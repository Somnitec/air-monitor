// Air Monitor — Phase 1 firmware.
//
// Goal (see DESIGN.md "Phase 1"): collect ONE record of every sensor at a fixed
// 60 s cadence, persist it durably to flash (survives power loss), and sync it to
// the LattePanda over WiFi when a connection is available. No adaptive sampling
// yet — Phase 1 is deliberately a clean, predictable baseline so we can measure
// power, sync reliability, and real sensor variance before tuning Phase 2.
//
// Durability model:
//   - Every record is appended to /queue.ndjson on LittleFS as one JSON line.
//   - /cursor.txt holds the byte offset the server has acknowledged.
//   - On sync we POST the un-acked tail; on HTTP 200 we advance the cursor.
//   - Once fully synced and the file is large, we truncate it (compaction).
// A sudden power cut loses at most the in-flight record, not the backlog.
//
// Build:  pio run -e esp32_phase1 -t upload    (then -t monitor)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>    // settimeofday() — adopt the server's clock when NTP is unavailable
#include <esp_system.h>  // esp_reset_reason()
#include <esp_task_wdt.h> // loop watchdog — reboots if a blocking call hangs the loop
#include <esp_sleep.h>   // esp_light_sleep_start() — CPU naps between captures in POWER_SAVING

#include <SensirionI2cSen66.h>
#include <Adafruit_ADXL345_U.h>
#include <BH1750.h>
#include <Adafruit_BME280.h>

#include "../../secrets.h"   // WIFI_*, SYNC_* (gitignored; see secrets.example.h)
#include "config.h"
#include "devconfig.h"
#include "mic.h"
#include "accel.h"
#include "record.h"
#include "ringstore.h"
#include "cadence.h"

// ---------------------------------------------------------------------------
// Sensor objects + presence flags
// ---------------------------------------------------------------------------
static SensirionI2cSen66        sen66;
static Adafruit_ADXL345_Unified adxl(12345);
static BH1750                   bh1750(ADDR_BH1750);
static Adafruit_BME280          bme;

static struct {
    bool sen66, adxl, bh1750, bme, soil, co, hcho, mic, battery;
} present;

// ---------------------------------------------------------------------------
// Known WiFi networks. WiFiMulti scans on connect and joins the strongest one
// that's in range, so the station roams between locations without reflashing.
// The list comes from secrets.h (WIFI_NETWORKS); if that's missing we fall back
// to the single WIFI_SSID/WIFI_PASSWORD so the file still builds.
// ---------------------------------------------------------------------------
static WiFiMulti wifiMulti;

// Random id regenerated every boot. Lets the server group a boot's records and
// back-fill the ones logged before the clock was known (see adoptServerTime).
static uint32_t g_bootId     = 0;
static uint32_t g_bootTs     = 0;   // UTC unix time of this boot (0 until NTP syncs)
static uint8_t  g_resetReason = 0;  // esp_reset_reason_t cast to uint8; set before Serial
static DeviceConfig g_config;       // device configuration (poll_interval_ms, etc.)

static const char* resetReasonStr(uint8_t r) {
    switch (r) {
        case 1:  return "POWERON";
        case 2:  return "EXT_PIN";
        case 3:  return "SW_RESET";
        case 4:  return "PANIC";
        case 5:  return "INT_WDT";
        case 6:  return "TASK_WDT";
        case 7:  return "WDT";
        case 8:  return "DEEPSLEEP";
        case 9:  return "BROWNOUT";
        case 10: return "SDIO";
        default: return "UNKNOWN";
    }
}

// Whether the ring was successfully initialised; guards ringstore_push in loop().
static bool s_ringReady = false;

// ---------------------------------------------------------------------------
// Operating mode + duty-cycle bookkeeping.
// ---------------------------------------------------------------------------
// NORMAL/TESTING are the mains-powered modes (WiFi always on). POWER_SAVING duty-
// cycles WiFi, light-sleeps between captures, gates the gas heaters + SEN66, and
// lowers the mic rate — see the POWER_SAVE_* block in config.h.
enum Mode { MODE_NORMAL, MODE_TESTING, MODE_POWER_SAVING };
static Mode     g_mode           = MODE_NORMAL;
static Mode     g_appliedMode    = MODE_NORMAL;   // last mode whose hardware transition ran
static uint32_t g_bootStartMs    = 0;
static uint32_t g_lastSyncAttempt = 0;

// Connection supervisor state. The device is unattended and all buffered data is
// durable, so recovery escalates: reconnect -> power-cycle radio -> reboot.
static uint32_t g_consecutiveSyncFail = 0;          // resets to 0 on any acked sync
static uint32_t g_lastGoodSyncMs      = 0;          // millis() of last acked sync (0 = never yet)
static volatile bool g_wifiDropped    = false;      // set by the WiFi event handler on disconnect

// Split-rate capture state. g_fields persists across loops so slow-channel values
// are carried forward between their (infrequent) re-reads; g_cadence drives adaptive
// storage; g_tSlow paces the slow channel.
static RecordFields g_fields;
static CadenceState g_cadence;
static uint32_t     g_tSlow = 0;

static bool inBootWindow() { return (millis() - g_bootStartMs) < BOOT_WINDOW_MS; }

static const char* modeStr(Mode m) {
    switch (m) {
        case MODE_TESTING:      return "testing";
        case MODE_POWER_SAVING: return "power_saving";
        default:                return "normal";
    }
}

// NORMAL/TESTING are mains-powered: keep WiFi connected continuously so uploads are
// immediate and the radio never misses the AP. POWER_SAVING duty-cycles the radio
// (off between syncs) to save power — but the boot window always keeps it up so a
// dashboard command (e.g. leave power-saving) still lands fast after a reboot.
static bool wifiShouldStayOn() {
    if (g_mode == MODE_POWER_SAVING) return inBootWindow();
    return true;
}

// Light-sleep the CPU (radio modem + most peripherals suspended, RAM retained) for
// `ms`, then resume. Used for the inter-capture gap and sensor warm-ups in
// POWER_SAVING so idle time costs ~mA instead of ~tens of mA. We feed the watchdog
// around it; naps are always far shorter than WDT_TIMEOUT_S.
static void powerNap(uint32_t ms) {
    if (ms == 0) return;
    esp_task_wdt_reset();
    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    esp_light_sleep_start();
    esp_task_wdt_reset();
}

struct WifiCred { const char* ssid; const char* pass; };
#ifdef WIFI_NETWORKS
static const WifiCred kWifiNetworks[] = WIFI_NETWORKS;
#else
static const WifiCred kWifiNetworks[] = { { WIFI_SSID, WIFI_PASSWORD } };
#endif
static const size_t kWifiCount = sizeof(kWifiNetworks) / sizeof(kWifiNetworks[0]);

// ---------------------------------------------------------------------------
// Small helpers (shared with the test sketches' approach)
// ---------------------------------------------------------------------------
static bool i2cAck(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}
static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)addr, 1);
    return Wire.available() ? Wire.read() : 0;
}
static uint32_t readAdcMv(int pin) {
    uint32_t acc = 0;
    for (int i = 0; i < GAS_ADC_OVERSAMPLE; ++i) acc += analogReadMilliVolts(pin);
    return acc / GAS_ADC_OVERSAMPLE;
}
// MEMS MOS sensor resistance from the load-resistor divider (docs/SENSORS.md).
static float gasRs(float vout_mv, float rl_ohms) {
    if (vout_mv < 1.0f) return INFINITY;
    return rl_ohms * (GAS_VCC_MV - vout_mv) / vout_mv;
}

// ---------------------------------------------------------------------------
// Time: NTP when WiFi is up; epoch is used as each record's timestamp.
// ---------------------------------------------------------------------------
static bool timeIsValid() {
    return (uint32_t)time(nullptr) > EPOCH_VALID_AFTER;
}
// Wall-clock prefix for serial logs, e.g. "14:03:21Z ". Empty until the clock is
// set (NTP or adopted server time), so logs stay readable before sync too.
static String clockStr() {
    if (!timeIsValid()) return String();
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%SZ ", &tm);
    return String(buf);
}
static void syncTimeIfNeeded() {
    if (timeIsValid()) return;
    // Throttle: each attempt blocks ~2 s and re-inits SNTP. Without this an offline
    // station (no internet, adopting time from the server reply instead) would stall
    // the loop on NTP every iteration. The first attempt runs immediately (s==0).
    static uint32_t s_lastNtpAttempt = 0;
    uint32_t now = millis();
    if (s_lastNtpAttempt != 0 && (now - s_lastNtpAttempt) < NTP_RETRY_INTERVAL_MS) return;
    s_lastNtpAttempt = now;
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC; tz handled on PC side
    // brief, non-blocking-ish wait; we don't stall the loop for long
    for (int i = 0; i < 20 && !timeIsValid(); ++i) delay(100);
    if (timeIsValid()) {
        uint32_t now = (uint32_t)time(nullptr);
        Serial.printf("[time] NTP ok: %lu\n", (unsigned long)now);
        if (g_bootTs == 0) {
            g_bootTs = now;
            Serial.printf("[boot] boot time: %lu\n", (unsigned long)g_bootTs);
        }
    }
}

// If our own clock is still unsynced (no NTP reachable), adopt the approximate
// time the server returns in its /ingest reply. Accurate to ~network latency,
// which is plenty for record timestamps. NTP, if it ever succeeds, takes over.
static void adoptServerTime(const String& resp) {
    if (timeIsValid()) return;
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return;            // not JSON / parse error
    uint32_t st = doc["server_time"] | 0;
    if (st > EPOCH_VALID_AFTER) {
        struct timeval tv = { .tv_sec = (time_t)st, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        Serial.printf("[time] adopted server time: %lu\n", (unsigned long)st);
    }
}

// ---------------------------------------------------------------------------
// Status LED (GPIO13). Idle = OFF. Blinks once at boot, then pulses briefly
// each time a sync batch is acknowledged by the server.
// ---------------------------------------------------------------------------
static void ledBlink(int n, int on_ms = 80, int off_ms = 80) {
    for (int i = 0; i < n; ++i) {
        digitalWrite(PIN_EXT_LED, HIGH); delay(on_ms);
        digitalWrite(PIN_EXT_LED, LOW);  if (i < n - 1) delay(off_ms);
    }
}
static void ledPulse(int on_ms = 60) {
    digitalWrite(PIN_EXT_LED, HIGH); delay(on_ms);
    digitalWrite(PIN_EXT_LED, LOW);
}

// ---------------------------------------------------------------------------
// WiFi: connect on demand, used both for NTP and for sync.
// ---------------------------------------------------------------------------
// Resolved server endpoint (set by mDNS discovery below; empty => SYNC_HOST
// fallback). Declared here so the recovery helpers can clear it on a radio reset.
static String   s_serverHost = "";          // resolved IP string; empty = use fallback
static uint16_t s_serverPort = SYNC_PORT;
static bool     s_mdnsUp     = false;

// Async WiFi events. We can't trust WiFi.status() alone — the stack can keep
// reporting WL_CONNECTED after the AP has gone away — so a disconnect event arms
// g_wifiDropped, which the supervisor in loop() uses to force a reconnect.
static void onWifiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            g_wifiDropped = false;
            WiFi.setSleep(false);   // re-assert: modem sleep black-holes LAN TCP
            Serial.printf("[wifi] got ip %s\n", WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            g_wifiDropped = true;
            Serial.println("[wifi] disconnected");
            break;
        default: break;
    }
}

// Last-resort radio reset: fully tear down and rebuild the WiFi stack. Clears a
// wedged DHCP/TCP state that survives a plain reconnect (the "still associated but
// nothing routes" failure seen in the field).
static void wifiHardCycle() {
    Serial.println("[wifi] hard radio cycle");
    WiFi.disconnect(true, true);    // disconnect + erase persisted config
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS);
    if (WiFi.status() == WL_CONNECTED) WiFi.setSleep(false);
    s_serverHost = "";              // force mDNS re-discovery after the reset
}

// Duty-cycle the radio off (POWER_SAVING, between syncs). The buffered ring is durable
// so nothing is lost; the next syncSession brings WiFi back up and re-discovers the
// server. Resets mDNS/host state since the lease/route may differ on the next wake.
static void wifiPowerDown() {
    WiFi.disconnect(true, false);   // disconnect + radio off, keep stored creds
    WiFi.mode(WIFI_OFF);
    s_serverHost = "";
    s_mdnsUp     = false;
    Serial.println("[wifi] radio off (power-saving)");
}

static bool wifiConnect() {
    if (WiFi.status() == WL_CONNECTED && !g_wifiDropped) return true;
    // wifiMulti.run() scans and connects to the strongest known network in range.
    wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS);
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok) {
        // Modem power-save makes the ESP32 miss frames the AP forwards between
        // clients — internet (via the gateway) still works but TCP to a LAN peer
        // black-holes. Keep the radio awake while we're up.
        WiFi.setSleep(false);
        Serial.printf("[wifi] connected: %s (%s, %d dBm)\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        Serial.printf("[wifi] gw=%s mask=%s dns=%s\n",
                      WiFi.gatewayIP().toString().c_str(),
                      WiFi.subnetMask().toString().c_str(),
                      WiFi.dnsIP().toString().c_str());
    } else {
        Serial.println("[wifi] no known network in range (will retry next cycle)");
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Server discovery via mDNS. The LattePanda advertises an "_airmon._tcp"
// service (pc/server.py does this with zeroconf), so we find it by name on
// whatever network we joined — no static IP. SYNC_HOST/SYNC_PORT from secrets.h
// are only a fallback if discovery turns up nothing. (s_serverHost/Port/mdnsUp are
// declared above so the recovery helpers can reach them.)
// ---------------------------------------------------------------------------
static void discoverServer() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!s_mdnsUp) s_mdnsUp = MDNS.begin(DEVICE_ID);   // our own hostname on the LAN
    if (!s_mdnsUp) return;

    int n = MDNS.queryService("airmon", "tcp");        // looks up _airmon._tcp.local.
    if (n > 0) {
        IPAddress ip = MDNS.IP(0);                     // queryService often returns 0.0.0.0...
        if (ip == IPAddress(0, 0, 0, 0)) {             // ...so resolve the A-record directly.
            ip = MDNS.queryHost("airmon-server");      // matches server.py ServiceInfo hostname
        }
        if (ip != IPAddress(0, 0, 0, 0)) {
            s_serverHost = ip.toString();
            s_serverPort = MDNS.port(0);
            Serial.printf("[mdns] found server: %s:%u\n", s_serverHost.c_str(), s_serverPort);
            return;
        }
    }
    // Keep any previously resolved host; otherwise the static SYNC_HOST fallback is used.
    Serial.println("[mdns] no usable _airmon record — using fallback SYNC_HOST");
}

// Reject physically-impossible SEN66 frames so corrupted reads never reach the DB.
// PM must be non-negative and monotonic (pm1≤pm2.5≤pm4≤pm10); RH 0–100 %; T a
// sane ambient range; VOC/NOx indices 0–500; CO2 0–40000 ppm.
static bool sen66ValuesSane(float pm1, float pm25, float pm4, float pm10,
                            float t, float rh, float voc, float nox, uint16_t co2) {
    if (pm1 < 0 || pm25 < pm1 || pm4 < pm25 || pm10 < pm4) return false;
    if (pm10 > 1000) return false;                    // SEN66 PM range tops out ~1000 µg/m³
    if (rh < 0 || rh > 100) return false;
    if (t < -40 || t > 85) return false;
    if (voc < 0 || voc > 500 || nox < 0 || nox > 500) return false;
    if (co2 > 40000) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Split-rate capture. The mic is the primary instrument and only listens ~1.3 s
// per capture, so the fast channel (mic + accel) is read every loop while the slow
// channel (air quality, weather, gas, soil, battery) is re-read only every few
// minutes — its values are carried forward into every record in between, so the
// stored series stays gapless without re-reading sensors that barely move.
// ---------------------------------------------------------------------------

// Read the fast channel (timestamp + mic + accel) into f, clearing their present
// flags first so a failed read this cycle shows as absent rather than stale.
static void readFast(RecordFields& f) {
    f.ts    = (uint32_t)time(nullptr);
    f.ts_ok = timeIsValid();
    f.up_ms = millis();
    f.boot  = (uint16_t)(g_bootId & 0xFFFF);

    f.status[GRP_ADXL] = FS_ABSENT;
    if (present.adxl) {
        AccelResult a;
        if (accel_capture(adxl, a)) {
            f.status[GRP_ADXL] = FS_OK;
            f.rumble_rms = a.rumble_rms; f.rumble_peak = a.rumble_peak; f.accel_mag = a.mag_mean;
            f.ppv_m_s      = a.ppv_m_s;
            f.accel_dom_hz = a.dom_freq_hz;
            for (int b = 0; b < REC_ACCEL_BANDS; ++b) f.accel_band_db[b] = a.band_db[b];
        } else {
            f.status[GRP_ADXL] = FS_INVALID;   // present but read failed — a real gap
        }
    }
    f.has_adxl = (f.status[GRP_ADXL] == FS_OK);

    f.status[GRP_MIC] = FS_ABSENT;
    if (present.mic) {
        MicResult m;
        if (mic_capture(m)) {
            f.status[GRP_MIC] = FS_OK;
            f.noise_dba  = m.laeq_est; f.noise_spl = m.spl_est; f.noise_dbfs = m.rms_dbfs;
            f.noise_clip = m.clipping;
            f.noise_lamax = m.lamax_dba;
            f.noise_lceq  = m.lceq;
            for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = m.band_dba[b];
        } else {
            f.status[GRP_MIC] = FS_INVALID;
        }
    }
    f.has_mic = (f.status[GRP_MIC] == FS_OK);
}

// Read the slow channel into f. On a failed/not-ready read the previous values are
// left in place (carry-forward) so the present flag and last good value persist.
static void readSlow(RecordFields& f) {
    // SEN66: present-but-not-ready / corrupt frame is FS_INVALID (a real gap), not
    // absent — so the server shows a NULL rather than carrying a stale value.
    if (!present.sen66) {
        f.status[GRP_SEN66] = FS_ABSENT;
    } else {
        uint8_t pad = 0; bool ready = false;
        bool okReady = (sen66.getDataReady(pad, ready) == 0) && ready;
        float pm1, pm25, pm4, pm10, t, rh, voc, nox; uint16_t co2;
        if (okReady && sen66.readMeasuredValues(pm1, pm25, pm4, pm10, rh, t, voc, nox, co2) == 0
            && sen66ValuesSane(pm1, pm25, pm4, pm10, t, rh, voc, nox, co2)) {
            f.status[GRP_SEN66] = FS_OK;
            f.pm1 = pm1; f.pm25 = pm25; f.pm4 = pm4; f.pm10 = pm10;
            f.co2 = co2; f.voc = voc; f.nox = nox; f.temp = t; f.rh = rh;
        } else {
            f.status[GRP_SEN66] = FS_INVALID;
        }
    }
    f.has_sen66 = (f.status[GRP_SEN66] == FS_OK);

    if (!present.bh1750)            f.status[GRP_BH1750] = FS_ABSENT;
    else if (bh1750.measurementReady(true)) { f.status[GRP_BH1750] = FS_OK; f.lux = bh1750.readLightLevel(); }
    else                           f.status[GRP_BH1750] = FS_UNCHANGED;  // not-ready is transient, carry last value
    f.has_bh1750 = (f.status[GRP_BH1750] == FS_OK);

    if (present.bme) {
        f.status[GRP_BME] = FS_OK;
        f.pressure = bme.readPressure() / 100.0f;
        f.bme_temp = bme.readTemperature();
        f.bme_rh   = bme.readHumidity();
    } else {
        f.status[GRP_BME] = FS_ABSENT;
    }
    f.has_bme = (f.status[GRP_BME] == FS_OK);

    // Analog channels: a present pin always yields a reading (no failure mode here).
    f.status[GRP_CO]   = present.co   ? FS_OK : FS_ABSENT;
    if (present.co)   f.co_mv   = (uint16_t)readAdcMv(PIN_GAS_CO_ADC);
    f.status[GRP_HCHO] = present.hcho ? FS_OK : FS_ABSENT;
    if (present.hcho) f.hcho_mv = (uint16_t)readAdcMv(PIN_GAS_HCHO_ADC);
    f.status[GRP_SOIL] = present.soil ? FS_OK : FS_ABSENT;
    if (present.soil) f.soil_mv = (uint16_t)readAdcMv(PIN_SOIL_ADC);
    f.status[GRP_BATTERY] = present.battery ? FS_OK : FS_ABSENT;
    if (present.battery) {
        f.bat_raw_mv = (uint16_t)readAdcMv(PIN_BATTERY_ADC);
        f.bat_cal = (bool)BAT_CALIBRATED;
    }
    f.has_co = present.co; f.has_hcho = present.hcho;
    f.has_soil = present.soil; f.has_battery = present.battery;
}

// Power-saving slow read: wake the duty-cycled slow sensors (SEN66 fan/laser + the
// analog gas-sensor heaters), light-sleep through their warm-up so the wait itself
// costs little, read, then return them to their low-power idle. Called instead of
// readSlow() only in MODE_POWER_SAVING; carry-forward between reads is unchanged.
static void readSlowPowerSaving(RecordFields& f) {
    bool gas = present.co || present.hcho;
    if (gas)           digitalWrite(PIN_GAS_HEATER_EN, HIGH);   // power the MEMS heaters
    if (present.sen66) sen66.startContinuousMeasurement();      // spin up fan + laser

    // Both need to stabilise; nap for the longer of the two warm-ups.
    uint32_t warm = GAS_HEATER_WARMUP_MS > SEN66_WARMUP_MS ? GAS_HEATER_WARMUP_MS
                                                           : SEN66_WARMUP_MS;
    powerNap(warm);

    readSlow(f);

    if (present.sen66) sen66.stopMeasurement();                 // back to idle current
    if (gas)           digitalWrite(PIN_GAS_HEATER_EN, LOW);    // cut heater power
}

// Apply the hardware side of a mode change (idempotent; runs once per transition).
// POWER_SAVING lowers the mic rate and parks the duty-cycled sensors in their idle
// state; the mains modes restore full fidelity and continuous sensors.
static void applyModeTransition() {
    if (g_mode == g_appliedMode) return;
    if (g_mode == MODE_POWER_SAVING) {
        if (present.mic)   mic_set_rate(POWER_SAVE_MIC_RATE_HZ);
        if (present.sen66) sen66.stopMeasurement();   // started per-read by readSlowPowerSaving
        digitalWrite(PIN_GAS_HEATER_EN, LOW);
        Serial.println("[power] -> POWER_SAVING: wifi duty-cycled, sensors gated, mic 16 kHz");
    } else {
        if (present.mic)   mic_set_rate(I2S_SAMPLE_RATE_HZ);
        if (present.sen66) sen66.startContinuousMeasurement();
        digitalWrite(PIN_GAS_HEATER_EN, HIGH);        // mains: heaters + fan run continuously
        if (g_appliedMode == MODE_POWER_SAVING)
            Serial.println("[power] left POWER_SAVING: wifi always-on, sensors continuous");
    }
    g_appliedMode = g_mode;
}

// Full read of every sensor (boot snapshot / one-shot diagnostics).
static void buildFields(RecordFields& f) {
    readSlow(f);
    readFast(f);
}

// On loop cycles where the slow channel is NOT re-read, downgrade its freshly-read
// (FS_OK) groups to FS_UNCHANGED: the value is carried forward in f and stays in the
// binary record, but record_to_json omits it so the server forward-fills it instead
// of the wire re-sending a value that hasn't been re-measured. FS_INVALID/FS_ABSENT
// are left as-is (a bad/absent sensor keeps reporting that until its next read).
static void markSlowUnchanged(RecordFields& f) {
    for (uint8_t g : { GRP_SEN66, GRP_BH1750, GRP_BME, GRP_CO, GRP_HCHO, GRP_SOIL, GRP_BATTERY })
        if (f.status[g] == FS_OK) f.status[g] = FS_UNCHANGED;
}

// Print a human-readable snapshot of all sensor values in f to Serial.
static void printFields(const RecordFields& f) {
    Serial.printf("%s[snap] ts=%u up=%.1fs\n", clockStr().c_str(), f.ts, f.up_ms / 1000.0f);
    if (f.has_sen66)
        Serial.printf("  SEN66   PM1=%.1f PM2.5=%.1f PM4=%.1f PM10=%.1f ug/m3"
                      "  CO2=%u ppm  VOC=%.0f  NOx=%.0f  T=%.1fC  RH=%.1f%%\n",
                      f.pm1, f.pm25, f.pm4, f.pm10, f.co2, f.voc, f.nox, f.temp, f.rh);
    if (f.has_bh1750)
        Serial.printf("  BH1750  lux=%.1f\n", f.lux);
    if (f.has_bme)
        Serial.printf("  BME280  T=%.1fC  RH=%.1f%%  P=%.1f hPa\n",
                      f.bme_temp, f.bme_rh, f.pressure);
    if (f.has_adxl) {
        Serial.printf("  ADXL345 rumble_rms=%.3f  peak=%.3f  mag=%.3f m/s²"
                      "  PPV=%.3f mm/s  dom=%u Hz\n",
                      f.rumble_rms, f.rumble_peak, f.accel_mag,
                      f.ppv_m_s * 1000.0f, f.accel_dom_hz);
        Serial.printf("          vib 4Hz=%.0f  8Hz=%.0f  16Hz=%.0f"
                      "  31Hz=%.0f  63Hz=%.0f  125Hz=%.0f dBm/s²\n",
                      f.accel_band_db[0], f.accel_band_db[1], f.accel_band_db[2],
                      f.accel_band_db[3], f.accel_band_db[4], f.accel_band_db[5]);
    }
    if (f.has_co)      Serial.printf("  CO      %u mV\n",   f.co_mv);
    if (f.has_hcho)    Serial.printf("  HCHO    %u mV\n",   f.hcho_mv);
    if (f.has_soil)    Serial.printf("  soil    %u mV\n",   f.soil_mv);
    if (f.has_battery) {
        float v   = f.bat_raw_mv / 1000.0f * BAT_DIVIDER_FACTOR;
        float pct = (v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f;
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        Serial.printf("  battery %.2f V  %.0f%%%s  (raw %u mV)\n",
                      v, pct, f.bat_cal ? "" : " uncal", f.bat_raw_mv);
    }
    if (f.has_mic)
        Serial.printf("  INMP441 LAeq=%.1f  LAmax=%.1f  LCeq=%.1f  LC-LA=%.1f  dBFS=%.1f%s\n",
                      f.noise_dba, f.noise_lamax, f.noise_lceq,
                      f.noise_lceq - f.noise_dba, f.noise_dbfs,
                      f.noise_clip ? "  CLIP" : "");
}

// Comprehensive latest-capture serial logging.
static void logCapturedValues(const RecordFields& f) {
    Serial.printf("%s[latest] ts=%u up=%u.%us\n", clockStr().c_str(), f.ts, f.up_ms / 1000, (f.up_ms % 1000) / 100);

    if (f.has_sen66)
        Serial.printf("  SEN66   PM2.5=%.1f PM10=%.1f ug/m3  CO2=%u ppm  T=%.1fC  RH=%.1f%%\n",
                      f.pm25, f.pm10, f.co2, f.temp, f.rh);

    if (f.has_bme)
        Serial.printf("  BME280  T=%.1fC  RH=%.1f%%  P=%.1f hPa\n",
                      f.bme_temp, f.bme_rh, f.pressure);

    if (f.has_bh1750)
        Serial.printf("  BH1750  lux=%.0f\n", f.lux);

    if (f.has_mic)
        Serial.printf("  INMP441 LAeq=%.1f  LAmax=%.1f  LCeq=%.1f  LC-LA=%.1f dB  dBFS=%.1f%s\n",
                      f.noise_dba, f.noise_lamax, f.noise_lceq,
                      f.noise_lceq - f.noise_dba, f.noise_dbfs,
                      f.noise_clip ? "  CLIP!" : "");

    if (f.has_adxl) {
        Serial.printf("  ADXL345 RMS=%.3f  peak=%.3f m/s²  PPV=%.2f mm/s  dom=%u Hz\n",
                      f.rumble_rms, f.rumble_peak, f.ppv_m_s * 1000.0f, f.accel_dom_hz);
        Serial.printf("          vib: 4Hz=%.0f  8Hz=%.0f  16Hz=%.0f  31Hz=%.0f  63Hz=%.0f  125Hz=%.0f dBm/s²\n",
                      f.accel_band_db[0], f.accel_band_db[1], f.accel_band_db[2],
                      f.accel_band_db[3], f.accel_band_db[4], f.accel_band_db[5]);
    }

    if (f.has_co)      Serial.printf("  CO      %u mV\n", f.co_mv);
    if (f.has_hcho)    Serial.printf("  HCHO    %u mV\n", f.hcho_mv);
    if (f.has_soil)    Serial.printf("  soil    %u mV\n", f.soil_mv);

    if (f.has_battery) {
        float v = f.bat_raw_mv / 1000.0f * BAT_DIVIDER_FACTOR;
        float pct = (v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f;
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        Serial.printf("  battery %.2f V  %.0f%%%s\n", v, pct, f.bat_cal ? "" : "  (uncal)");
    }
}

// ---------------------------------------------------------------------------
// Duty-cycled sync session + dashboard-driven mode state machine.
// ---------------------------------------------------------------------------

// Apply a {"command":{"set_mode": "..."}} returned by the server.
static void applyServerCommand(const JsonDocument& doc) {
    const char* sm = doc["command"]["set_mode"] | (const char*)nullptr;
    if (!sm) return;
    Mode want = g_mode;
    if      (!strcmp(sm, "testing"))      want = MODE_TESTING;
    else if (!strcmp(sm, "normal"))       want = MODE_NORMAL;
    else if (!strcmp(sm, "power_saving")) want = MODE_POWER_SAVING;
    else return;
    if (want != g_mode) {
        g_mode = want;   // hardware transition (mic rate, SEN66, heater) runs in loop()
        Serial.printf("[mode] -> %s (server)\n", modeStr(g_mode));
    }
}

// Apply device config from server response, if present.
static void applyServerConfig(const JsonDocument& doc) {
    if (!doc["config"].is<JsonObject>()) return;
    String cfg_str;
    serializeJson(doc["config"], cfg_str);
    devconfig_apply_json(cfg_str.c_str(), g_config);
}

// Ordered list of server endpoints to try, so all three deployment modes work
// without reconfiguration:
//   1. the mDNS-resolved host (normal LAN / an external AP shared with the server)
//   2. the static SYNC_HOST from secrets.h (hardcoded fallback)
//   3. the WiFi gateway — when the SERVER itself runs the hotspot, it *is* the
//      gateway, so this nails the laptop-as-hotspot case even if mDNS never resolves.
// Duplicates are dropped. The winner is pinned into s_serverHost so subsequent
// batches go straight to it (one attempt) until it fails.
struct Endpoint { String host; uint16_t port; };
static uint8_t buildEndpoints(Endpoint out[3]) {
    uint8_t nc = 0;
    auto add = [&](const String& h, uint16_t p) {
        if (!h.length() || h == "0.0.0.0") return;
        for (uint8_t i = 0; i < nc; ++i) if (out[i].host == h) return;  // dedupe
        out[nc++] = { h, p };
    };
    if (s_serverHost.length()) add(s_serverHost, s_serverPort);
    add(String(SYNC_HOST), (uint16_t)SYNC_PORT);
    IPAddress gw = WiFi.gatewayIP();
    if (gw != IPAddress(0, 0, 0, 0)) add(gw.toString(), (uint16_t)SYNC_PORT);
    return nc;
}

// One HTTP POST of a pre-serialized body to a single endpoint. Returns the HTTP code
// (<=0 on transport failure) and fills `resp` with the body on a 2xx.
static int httpPostTo(const String& host, uint16_t port, const String& body, String& resp) {
    HTTPClient http;
    String url = String("http://") + host + ":" + String(port) + SYNC_PATH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);
    int code = http.POST(body);
    resp = http.getString();
    http.end();
    return code;
}

// POST one batch of records as an envelope. Returns the HTTP code; on success
// advances the synced pointer and applies any server command.
static int postBatch(const Record* recs, uint32_t n, uint32_t lastSeq) {
    JsonDocument doc;
    doc["dev"]          = DEVICE_ID;
    doc["boot"]         = g_bootId;
    doc["boot_ts"]      = g_bootTs;
    doc["reset_reason"] = resetReasonStr(g_resetReason);
    doc["fw"]           = "phase1";
    doc["mode"]         = modeStr(g_mode);
    doc["buffered"]     = ringstore_unsynced();
    JsonArray arr = doc["records"].to<JsonArray>();
    for (uint32_t i = 0; i < n; ++i) {
        JsonDocument tmp;
        // First record of the batch carries a FULL slow snapshot (not delta-encoded)
        // so the server reconstructs correctly even with a cold forward-fill cache —
        // the reconnect-after-outage case where the backlog would otherwise start with
        // NULL PM/CO2/temp/… until the next fresh read.
        record_to_json(recs[i], tmp, /*full_slow=*/ i == 0);
        arr.add(tmp);
    }
    String body; serializeJson(doc, body);

    Endpoint eps[3];
    uint8_t nep = buildEndpoints(eps);
    int lastCode = -1;
    for (uint8_t i = 0; i < nep; ++i) {
        esp_task_wdt_reset();   // trying several endpoints can take a few seconds
        String resp;
        int code = httpPostTo(eps[i].host, eps[i].port, body, resp);
        if (code == 200 || code == 201 || code == 204) {
            s_serverHost = eps[i].host;   // pin the working endpoint for next time
            s_serverPort = eps[i].port;
            JsonDocument rdoc;
            if (!deserializeJson(rdoc, resp)) {
                adoptServerTime(resp);
                applyServerCommand(rdoc);
                applyServerConfig(rdoc);
            }
            ringstore_mark_synced(lastSeq);
            g_consecutiveSyncFail = 0;
            g_lastGoodSyncMs      = millis();
            Serial.printf("%s[sync] %u acked via %s:%u (unsynced now %u)\n",
                          clockStr().c_str(), n, eps[i].host.c_str(), eps[i].port,
                          ringstore_unsynced());
            ledPulse();
            return code;
        }
        lastCode = code;
    }
    Serial.printf("[sync] POST failed (last code=%d, %u endpoint(s)) — server unreachable,"
                  " latest values over USB:\n", lastCode, nep);
    if (n > 0) { RecordFields lf; record_unpack(recs[n - 1], lf); logCapturedValues(lf); }
    g_consecutiveSyncFail++;
    s_serverHost = "";     // force re-discovery next time
    return lastCode;
}

// Drain the ring to the server in batches until empty or a POST fails.
static void drainRing() {
    if (WiFi.status() != WL_CONNECTED) return;
    Record batch[SYNC_BATCH_MAX];
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t lastSeq = 0;
        uint32_t n = ringstore_drain(batch, SYNC_BATCH_MAX, &lastSeq);
        if (n == 0) break;
        int code = postBatch(batch, n, lastSeq);
        // Stop on any non-2xx: >=400 is a server reject, <=0 is a transport failure
        // (connection refused / not connected). Retrying the same batch in a tight
        // loop just burns the watchdog budget — the supervisor handles recovery.
        if (code < 200 || code >= 400 || WiFi.status() != WL_CONNECTED) break;
        if (ringstore_unsynced() == 0) break;
    }
}

// Bring WiFi up, sync time/discover, drain.
// WiFi stays on always — the device is mains-powered so duty-cycling gains nothing
// and causes the visible connect/disconnect churn on the network.
static void syncSession() {
    g_lastSyncAttempt = millis();

    // Escalation step 1: after repeated failures, a plain reconnect isn't enough —
    // power-cycle the radio to clear a wedged DHCP/TCP stack before trying again.
    if (g_consecutiveSyncFail >= WIFI_HARD_CYCLE_FAIL) {
        wifiHardCycle();
        g_consecutiveSyncFail = 0;   // give the fresh stack a clean slate of attempts
    }

    if (wifiConnect()) {
        syncTimeIfNeeded();
        if (s_serverHost.length() == 0) discoverServer();
        drainRing();
        // POWER_SAVING: turn the radio back off once drained (outside the boot window,
        // where we keep it up so dashboard commands still land promptly).
        if (g_mode == MODE_POWER_SAVING && !inBootWindow()) wifiPowerDown();
    }

    // Escalation step 2: if nothing has acked for a long time, reboot. All buffered
    // data is durable on flash and we resume from the persisted cursor, so this is a
    // safe last resort for any failure a radio cycle can't clear (hung stack, bad
    // DNS, server moved). g_lastGoodSyncMs==0 means we've never synced yet — anchor
    // the stall window to boot so a never-reachable server still triggers a reboot.
    uint32_t since = millis() - (g_lastGoodSyncMs ? g_lastGoodSyncMs : g_bootStartMs);
    if (since >= STALL_REBOOT_MS) {
        Serial.printf("[sync] no acked sync for %lu ms — rebooting to recover\n",
                      (unsigned long)since);
        Serial.flush();
        ESP.restart();
    }
}

// ---------------------------------------------------------------------------
// Sensor bring-up (same probing approach as main.cpp's selfTest)
// ---------------------------------------------------------------------------
static void initSensors() {
    Serial.println("[init] probing sensors...");

    if (i2cAck(ADDR_SEN66)) {
        sen66.begin(Wire, ADDR_SEN66);
        present.sen66 = (sen66.startContinuousMeasurement() == 0);
    }
    Serial.printf("  SEN66:   %s\n", present.sen66 ? "ok" : "absent");

    if (i2cAck(ADDR_ADXL345) && i2cReadReg(ADDR_ADXL345, 0x00) == ADXL345_DEVID
        && adxl.begin(ADDR_ADXL345)) {
        adxl.setRange(ADXL345_RANGE_4_G);
        present.adxl = true;
    }
    Serial.printf("  ADXL345: %s\n", present.adxl ? "ok" : "absent");

    if (i2cAck(ADDR_BH1750)) {
        // HIGH_RES_MODE_2 = 0.5 lx steps; raising MTreg to its max (254) pushes the
        // resolution to ~0.11 lx so dim/near-dark light (moonlight, standby LEDs) is
        // measurable instead of flooring at 0. Trade-off: max range drops to ~7000 lx,
        // so only direct sunlight saturates — fine for this indoor/low-light station.
        present.bh1750 = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE_2, ADDR_BH1750);
        if (present.bh1750) bh1750.setMTreg(254);
    }
    Serial.printf("  BH1750:  %s\n", present.bh1750 ? "ok" : "absent");

    if (i2cAck(ADDR_BME280) && i2cReadReg(ADDR_BME280, 0xD0) == BME280_CHIPID) {
        present.bme = bme.begin(ADDR_BME280, &Wire);
    }
    Serial.printf("  BME280:  %s\n", present.bme ? "ok" : "absent");

    // Analog "presence" = plausible idle voltage (can't truly detect).
    auto plausible = [](int pin) {
        uint32_t mv = readAdcMv(pin);
        return mv >= ANALOG_PRESENT_MIN_MV && mv <= ANALOG_PRESENT_MAX_MV;
    };
    present.soil = plausible(PIN_SOIL_ADC);
    present.co   = plausible(PIN_GAS_CO_ADC);
    present.hcho = plausible(PIN_GAS_HCHO_ADC);
    Serial.printf("  soil/CO/HCHO: %d/%d/%d\n", present.soil, present.co, present.hcho);

#if BATTERY_ENABLED
    present.battery = true;   // we log raw mV regardless; calibration is separate
    Serial.printf("  battery: enabled on GPIO%d (cal=%d)\n", PIN_BATTERY_ADC, BAT_CALIBRATED);
#else
    Serial.println("  battery: disabled");
#endif

    float dbfs;
    if (mic_begin() && mic_selftest(dbfs)) present.mic = true;
    Serial.printf("  INMP441: %s\n", present.mic ? "ok" : "absent");
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    pinMode(PIN_EXT_LED, OUTPUT);
    digitalWrite(PIN_EXT_LED, LOW);

    // Gas-heater MOSFET enable: default ON so NORMAL/TESTING run the heaters
    // continuously (mains). POWER_SAVING gates this low between reads. Harmless on the
    // current board (no MOSFET yet) — just drives a free GPIO.
    pinMode(PIN_GAS_HEATER_EN, OUTPUT);
    digitalWrite(PIN_GAS_HEATER_EN, HIGH);

    Serial.begin(115200);
    delay(300);
    g_resetReason = (uint8_t)esp_reset_reason();
    g_bootId = esp_random();
    Serial.printf("\n[boot] air-monitor phase 1 (boot=%lu)\n", (unsigned long)g_bootId);
    Serial.printf("[boot] reset reason: %s (%u)\n", resetReasonStr(g_resetReason), g_resetReason);

    bool s_fsMounted = LittleFS.begin(true);   // format on first boot if needed
    if (!s_fsMounted) {
        Serial.println("[fs] LittleFS mount failed! Run: pio run -t uploadfs");
        // Leave s_ringReady false; ringstore_begin is never called below.
    }

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_SOIL_ADC,     ADC_11db);
    analogSetPinAttenuation(PIN_GAS_CO_ADC,   ADC_11db);
    analogSetPinAttenuation(PIN_GAS_HCHO_ADC, ADC_11db);
#if BATTERY_ENABLED
    analogSetPinAttenuation(PIN_BATTERY_ADC,  ADC_11db);
#endif

    initSensors();

    // Register every known network with WiFiMulti.
    WiFi.persistent(false);          // don't thrash flash storing creds every connect
    WiFi.setAutoReconnect(true);     // let the core retry associations on its own
    WiFi.onEvent(onWifiEvent);       // arm g_wifiDropped on disconnect / clear on GOT_IP
    WiFi.mode(WIFI_STA);
    for (size_t i = 0; i < kWifiCount; ++i)
        wifiMulti.addAP(kWifiNetworks[i].ssid, kWifiNetworks[i].pass);
    Serial.printf("[wifi] %u known network(s) registered\n", (unsigned)kWifiCount);

    // Loop watchdog: reboot if a blocking call (HTTP/mDNS/I2S) hangs the loop past
    // WDT_TIMEOUT_S. Durable ring => the reboot resumes with no data loss.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    g_bootStartMs = millis();
    if (s_fsMounted) {
        s_ringReady = ringstore_begin();
        if (!s_ringReady) Serial.println("[ring] begin failed — samples will not be stored");
        g_config = devconfig_load();
        Serial.printf("[config] poll_interval_ms = %u\n", g_config.poll_interval_ms);
    }

    // Boot window: WiFi on so the dashboard can flip us into testing mode.
    if (wifiConnect()) { syncTimeIfNeeded(); discoverServer(); }

    ledBlink(3);
    digitalWrite(PIN_EXT_LED, LOW);
    Serial.println("[boot] ready — sampling every "
                   + String(g_config.poll_interval_ms / 1000.0f) + " s");
    RecordFields snap; buildFields(snap);
    printFields(snap);
}

void loop() {
    const uint32_t now = millis();

    esp_task_wdt_reset();   // we're alive — defer the watchdog reboot

    applyModeTransition();  // run hardware side of any pending mode change once

    if (wifiShouldStayOn() && WiFi.status() != WL_CONNECTED) wifiConnect();
    if (WiFi.status() == WL_CONNECTED) { syncTimeIfNeeded();
        if (s_serverHost.length() == 0) discoverServer(); }

    // --- fast channel: capture noise + accel every loop (mic_capture paces us
    //     at ~1.3 s, the natural listen window). ---
    readFast(g_fields);

    // Adaptive storage: densify when the level moves fast, decimate when quiet.
    // quiet_store_ms tracks the server-set poll_interval_ms so the dashboard's
    // cadence knob still governs the quiet baseline. TESTING stores every capture.
    CadenceParams cp{ NOISE_DENSIFY_DELTA_DBA, DENSIFY_HOLD_MS, g_config.poll_interval_ms };
    CadenceDecision cd = cadence_decide(g_cadence, cp, g_fields.has_mic, g_fields.noise_dba, now);
    bool store = (g_mode == MODE_TESTING) ? true : cd.store;

    // --- slow channel: re-read air-quality/weather/gas/soil/battery on their own
    //     cadence (faster while densified), carried forward into every record. ---
    uint32_t slowInterval = cd.densified ? SLOW_INTERVAL_DENSE_MS : SLOW_INTERVAL_MS;
    if (g_tSlow == 0 || now - g_tSlow >= slowInterval) {
        // POWER_SAVING wakes/warms/parks the duty-cycled sensors around the read.
        if (g_mode == MODE_POWER_SAVING) readSlowPowerSaving(g_fields);
        else                             readSlow(g_fields);   // fresh -> FS_OK (or INVALID/ABSENT)
        g_tSlow = now;
    } else {
        markSlowUnchanged(g_fields);  // carry forward -> FS_UNCHANGED (omitted on wire)
    }

    if (store) {
        logCapturedValues(g_fields);  // print latest values to serial
        Record rec = record_pack(g_fields);
        if (s_ringReady) {
            ringstore_push(rec);
            Serial.printf("%s[rec] seq=%u ts=%u buffered=%u mode=%s%s\n",
                          clockStr().c_str(), rec.seq, rec.ts, ringstore_unsynced(),
                          modeStr(g_mode), cd.densified ? " dense" : "");
        } else {
            Serial.printf("[rec] ts=%u (ring not ready, discarding)\n", rec.ts);
        }

        // TESTING: push live right after each sample.
        if (g_mode == MODE_TESTING && WiFi.status() == WL_CONNECTED) drainRing();
    }

    // --- decide whether to run a sync session ---
    if (g_mode == MODE_TESTING) {
        // stay connected; live drain already handled at sample time
    } else if (inBootWindow()) {
        // keep WiFi up and poll the server so an "enter testing" command lands fast
        if (now - g_lastSyncAttempt >= 5000) syncSession();
    } else {
        // POWER_SAVING syncs far less often (radio up for the drain, then off again).
        uint32_t syncIntvl = (g_mode == MODE_POWER_SAVING) ? POWER_SAVE_SYNC_INTERVAL_MS
                                                           : SYNC_ATTEMPT_INTERVAL_MS;
        bool dueByTime  = (now - g_lastSyncAttempt) >= syncIntvl;
        bool dueByCount = ringstore_unsynced() >= SYNC_THRESHOLD_RECORDS;
        if (g_lastSyncAttempt == 0 || dueByTime || dueByCount) syncSession();
    }

    // POWER_SAVING: with the radio off between syncs, light-sleep the inter-capture
    // gap so idle current drops from ~tens of mA to ~mA. Skip while WiFi must stay up
    // (boot window / mid-sync) — light sleep would suspend the modem and stall the
    // drain. Other modes keep the original tiny busy-delay.
    if (g_mode == MODE_POWER_SAVING && !wifiShouldStayOn()
        && WiFi.status() != WL_CONNECTED && POWER_SAVE_CAPTURE_GAP_MS > 0) {
        powerNap(POWER_SAVE_CAPTURE_GAP_MS);
    } else {
        delay(50);
    }
}
