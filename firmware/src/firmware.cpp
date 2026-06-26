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

#include <SensirionI2cSen66.h>
#include <Adafruit_ADXL345_U.h>
#include <BH1750.h>
#include <Adafruit_BME280.h>

#include "secrets.h"     // WIFI_*, SYNC_* (gitignored; see secrets.example.h)
#include "config.h"
#include "mic.h"
#include "accel.h"
#include "record.h"
#include "ringstore.h"

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
static uint32_t g_bootId = 0;

// ---------------------------------------------------------------------------
// Operating mode + duty-cycle bookkeeping.
// ---------------------------------------------------------------------------
enum Mode { MODE_NORMAL, MODE_TESTING };
static Mode     g_mode           = MODE_NORMAL;
static uint32_t g_bootStartMs    = 0;
static uint32_t g_lastSyncAttempt = 0;

static bool inBootWindow() { return (millis() - g_bootStartMs) < BOOT_WINDOW_MS; }
static bool wifiShouldStayOn() { return g_mode == MODE_TESTING || inBootWindow(); }

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
static void syncTimeIfNeeded() {
    if (timeIsValid()) return;
    configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC; tz handled on PC side
    // brief, non-blocking-ish wait; we don't stall the loop for long
    for (int i = 0; i < 20 && !timeIsValid(); ++i) delay(100);
    if (timeIsValid()) Serial.printf("[time] NTP ok: %lu\n", (unsigned long)time(nullptr));
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
static bool wifiConnect() {
    if (WiFi.status() == WL_CONNECTED) return true;
    // wifiMulti.run() scans and connects to the strongest known network in range.
    wifiMulti.run(WIFI_CONNECT_TIMEOUT_MS);
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok) Serial.printf("[wifi] connected: %s (%s, %d dBm)\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else    Serial.println("[wifi] no known network in range (will retry next cycle)");
    return ok;
}

// ---------------------------------------------------------------------------
// Server discovery via mDNS. The LattePanda advertises an "_airmon._tcp"
// service (pc/server.py does this with zeroconf), so we find it by name on
// whatever network we joined — no static IP. SYNC_HOST/SYNC_PORT from secrets.h
// are only a fallback if discovery turns up nothing.
// ---------------------------------------------------------------------------
static String   s_serverHost = "";          // resolved IP string; empty = use fallback
static uint16_t s_serverPort = SYNC_PORT;
static bool     s_mdnsUp     = false;

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

// ---------------------------------------------------------------------------
// Build one RecordFields from a fresh read of every present sensor.
// ---------------------------------------------------------------------------
static void buildFields(RecordFields& f) {
    f.ts    = (uint32_t)time(nullptr);
    f.ts_ok = timeIsValid();
    f.up_ms = millis();
    f.boot  = (uint16_t)(g_bootId & 0xFFFF);

    if (present.sen66) {
        float pm1, pm25, pm4, pm10, t, rh, voc, nox; uint16_t co2;
        if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, rh, t, voc, nox, co2) == 0) {
            f.has_sen66 = true;
            f.pm1 = pm1; f.pm25 = pm25; f.pm4 = pm4; f.pm10 = pm10;
            f.co2 = co2; f.voc = voc; f.nox = nox; f.temp = t; f.rh = rh;
        }
    }
    if (present.bh1750 && bh1750.measurementReady(true)) {
        f.has_bh1750 = true; f.lux = bh1750.readLightLevel();
    }
    if (present.bme) {
        f.has_bme = true;
        f.pressure = bme.readPressure() / 100.0f;
        f.bme_temp = bme.readTemperature();
        f.bme_rh   = bme.readHumidity();
    }
    if (present.adxl) {
        AccelResult a;
        if (accel_capture(adxl, a)) {
            f.has_adxl = true;
            f.rumble_rms = a.rumble_rms; f.rumble_peak = a.rumble_peak; f.accel_mag = a.mag_mean;
        }
    }
    if (present.co)   { f.has_co = true;   f.co_mv   = (uint16_t)readAdcMv(PIN_GAS_CO_ADC); }
    if (present.hcho) { f.has_hcho = true; f.hcho_mv = (uint16_t)readAdcMv(PIN_GAS_HCHO_ADC); }
    if (present.soil) { f.has_soil = true; f.soil_mv = (uint16_t)readAdcMv(PIN_SOIL_ADC); }
    if (present.battery) {
        f.has_battery = true;
        f.bat_raw_mv = (uint16_t)readAdcMv(PIN_BATTERY_ADC);
        f.bat_cal = (bool)BAT_CALIBRATED;
    }
    if (present.mic) {
        MicResult m;
        if (mic_capture(m)) {
            f.has_mic = true;
            f.noise_dba = m.laeq_est; f.noise_spl = m.spl_est; f.noise_dbfs = m.rms_dbfs;
            f.noise_clip = m.clipping;
            for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = m.band_dba[b];
        }
    }
}

// ---------------------------------------------------------------------------
// Duty-cycled sync session + dashboard-driven mode state machine.
// ---------------------------------------------------------------------------

// Apply a {"command":{"set_mode": "..."}} returned by the server.
static void applyServerCommand(const JsonDocument& doc) {
    const char* sm = doc["command"]["set_mode"] | (const char*)nullptr;
    if (!sm) return;
    if (!strcmp(sm, "testing") && g_mode != MODE_TESTING) {
        g_mode = MODE_TESTING; Serial.println("[mode] -> TESTING (server)");
    } else if (!strcmp(sm, "normal") && g_mode != MODE_NORMAL) {
        g_mode = MODE_NORMAL;  Serial.println("[mode] -> NORMAL (server)");
    }
}

// POST one batch of records as an envelope. Returns the HTTP code; on success
// advances the synced pointer and applies any server command.
static int postBatch(const Record* recs, uint32_t n, uint32_t lastSeq) {
    String   host = s_serverHost.length() ? s_serverHost : String(SYNC_HOST);
    uint16_t port = s_serverHost.length() ? s_serverPort : (uint16_t)SYNC_PORT;

    JsonDocument doc;
    doc["dev"]      = DEVICE_ID;
    doc["boot"]     = g_bootId;
    doc["fw"]       = "phase1";
    doc["mode"]     = (g_mode == MODE_TESTING) ? "testing" : "normal";
    doc["buffered"] = ringstore_unsynced();
    JsonArray arr = doc["records"].to<JsonArray>();
    for (uint32_t i = 0; i < n; ++i) {
        JsonDocument tmp;
        record_to_json(recs[i], tmp);
        arr.add(tmp);
    }
    String body; serializeJson(doc, body);

    HTTPClient http;
    String url = String("http://") + host + ":" + String(port) + SYNC_PATH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (code == 200 || code == 201 || code == 204) {
        JsonDocument rdoc;
        if (!deserializeJson(rdoc, resp)) { adoptServerTime(resp); applyServerCommand(rdoc); }
        ringstore_mark_synced(lastSeq);
        Serial.printf("[sync] %u acked (unsynced now %u)\n", n, ringstore_unsynced());
        ledPulse();
        return code;
    }
    Serial.printf("[sync] POST failed code=%d\n", code);
    s_serverHost = "";     // force re-discovery next time
    return code;
}

// Drain the ring to the server in batches until empty or a POST fails.
static void drainRing() {
    if (WiFi.status() != WL_CONNECTED) return;
    Record batch[SYNC_BATCH_MAX];
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t lastSeq = 0;
        uint32_t n = ringstore_drain(batch, SYNC_BATCH_MAX, &lastSeq);
        if (n == 0) break;
        if (postBatch(batch, n, lastSeq) >= 400 || WiFi.status() != WL_CONNECTED) break;
        if (ringstore_unsynced() == 0) break;
    }
}

// Bring WiFi up, sync time/discover, drain, and (in NORMAL, outside the boot
// window) drop WiFi to save power.
static void syncSession() {
    g_lastSyncAttempt = millis();
    if (wifiConnect()) {
        syncTimeIfNeeded();
        if (s_serverHost.length() == 0) discoverServer();
        drainRing();
    }
    if (g_mode == MODE_NORMAL && !inBootWindow()) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
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
        present.bh1750 = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, ADDR_BH1750);
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

    Serial.begin(115200);
    delay(300);
    g_bootId = esp_random();
    Serial.printf("\n[boot] air-monitor phase 1 (boot=%lu)\n", (unsigned long)g_bootId);

    if (!LittleFS.begin(true)) {        // format on first boot if needed
        Serial.println("[fs] LittleFS mount failed!");
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
    WiFi.mode(WIFI_STA);
    for (size_t i = 0; i < kWifiCount; ++i)
        wifiMulti.addAP(kWifiNetworks[i].ssid, kWifiNetworks[i].pass);
    Serial.printf("[wifi] %u known network(s) registered\n", (unsigned)kWifiCount);

    g_bootStartMs = millis();
    if (!ringstore_begin()) Serial.println("[ring] begin failed");

    // Boot window: WiFi on so the dashboard can flip us into testing mode.
    if (wifiConnect()) { syncTimeIfNeeded(); discoverServer(); }

    ledBlink(3);
    digitalWrite(PIN_EXT_LED, LOW);
    Serial.println("[boot] ready — sampling every "
                   + String(SAMPLE_BASELINE_MS / 1000) + " s");
}

void loop() {
    static uint32_t tSample = 0;
    const uint32_t now = millis();

    // --- fixed-cadence baseline sample ---
    if (tSample == 0 || now - tSample >= SAMPLE_BASELINE_MS) {
        tSample = now;

        if (wifiShouldStayOn() && WiFi.status() != WL_CONNECTED) wifiConnect();
        if (WiFi.status() == WL_CONNECTED) { syncTimeIfNeeded();
            if (s_serverHost.length() == 0) discoverServer(); }

        RecordFields f; buildFields(f);
        Record rec = record_pack(f);
        ringstore_push(rec);
        Serial.printf("[rec] seq=%u ts=%u buffered=%u mode=%s\n",
                      rec.seq, rec.ts, ringstore_unsynced(),
                      g_mode == MODE_TESTING ? "testing" : "normal");

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
        bool dueByTime  = (now - g_lastSyncAttempt) >= SYNC_ATTEMPT_INTERVAL_MS;
        bool dueByCount = ringstore_unsynced() >= SYNC_THRESHOLD_RECORDS;
        if (g_lastSyncAttempt == 0 || dueByTime || dueByCount) syncSession();
    }

    delay(50);
}
