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

#include <SensirionI2cSen66.h>
#include <Adafruit_ADXL345_U.h>
#include <BH1750.h>

#include "secrets.h"     // WIFI_*, SYNC_* (gitignored; see secrets.example.h)
#include "config.h"
#include "mic.h"
#include "accel.h"

// ---------------------------------------------------------------------------
// Sensor objects + presence flags
// ---------------------------------------------------------------------------
static SensirionI2cSen66        sen66;
static Adafruit_ADXL345_Unified adxl(12345);
static BH1750                   bh1750(ADDR_BH1750);

static struct {
    bool sen66, adxl, bh1750, soil, co, hcho, mic, battery;
} present;

// ---------------------------------------------------------------------------
// Known WiFi networks. WiFiMulti scans on connect and joins the strongest one
// that's in range, so the station roams between locations without reflashing.
// The list comes from secrets.h (WIFI_NETWORKS); if that's missing we fall back
// to the single WIFI_SSID/WIFI_PASSWORD so the file still builds.
// ---------------------------------------------------------------------------
static WiFiMulti wifiMulti;
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
        s_serverHost = MDNS.IP(0).toString();
        s_serverPort = MDNS.port(0);
        Serial.printf("[mdns] found server: %s:%u\n", s_serverHost.c_str(), s_serverPort);
    } else {
        Serial.println("[mdns] no _airmon server found — using fallback SYNC_HOST");
    }
}

// ---------------------------------------------------------------------------
// Durable queue: append NDJSON, track an acknowledged byte cursor.
// ---------------------------------------------------------------------------
static size_t cursorRead() {
    File f = LittleFS.open(QUEUE_CURSOR_PATH, "r");
    if (!f) return 0;
    String s = f.readStringUntil('\n');
    f.close();
    return (size_t)s.toInt();
}
static void cursorWrite(size_t pos) {
    File f = LittleFS.open(QUEUE_CURSOR_PATH, "w");
    if (!f) return;
    f.println(pos);
    f.close();
}
static void queueAppendLine(const String& line) {
    File f = LittleFS.open(QUEUE_PATH, "a");
    if (!f) { Serial.println("[queue] append failed (FS full?)"); return; }
    f.println(line);            // NDJSON: one record per line
    f.close();
}
// Once the server has acked everything and the file has grown past the compaction
// threshold, drop it and reset the cursor to reclaim flash.
static void queueCompactIfDone() {
    File f = LittleFS.open(QUEUE_PATH, "r");
    if (!f) return;
    size_t sz = f.size();
    f.close();
    if (sz >= QUEUE_COMPACT_BYTES && cursorRead() >= sz) {
        LittleFS.remove(QUEUE_PATH);
        cursorWrite(0);
        Serial.println("[queue] compacted (all synced)");
    }
}

// ---------------------------------------------------------------------------
// Build one record as a JSON object string.
// Field names are short but readable; the PC stores them verbatim in a JSON blob.
// ---------------------------------------------------------------------------
static String buildRecord() {
    JsonDocument doc;

    uint32_t now = (uint32_t)time(nullptr);
    doc["ts"]      = now;                 // unix epoch (UTC). < EPOCH_VALID_AFTER => clock unsynced
    doc["ts_ok"]   = timeIsValid();       // PC can quarantine records with a bad clock
    doc["dev"]     = DEVICE_ID;
    doc["up_ms"]   = millis();            // uptime, for power/restart diagnostics

    // ---- SEN66: PM / CO2 / VOC / NOx / T / RH ----
    if (present.sen66) {
        float pm1, pm25, pm4, pm10, t, rh, voc, nox; uint16_t co2;
        if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, t, rh, voc, nox, co2) == 0) {
            doc["pm1"]  = pm1;  doc["pm25"] = pm25; doc["pm4"] = pm4; doc["pm10"] = pm10;
            doc["co2"]  = co2;  doc["voc"]  = voc;  doc["nox"] = nox;
            doc["temp"] = t;    doc["rh"]   = rh;
        }
    }

    // ---- BH1750: ambient light ----
    if (present.bh1750 && bh1750.measurementReady(true)) {
        doc["lux"] = bh1750.readLightLevel();
    }

    // ---- ADXL345: ground-rumble level (mic-style) ----
    if (present.adxl) {
        AccelResult a;
        if (accel_capture(adxl, a)) {
            doc["rumble"]      = a.rumble_rms;    // AC RMS, m/s^2
            doc["rumble_peak"] = a.rumble_peak;
            doc["accel_mag"]   = a.mag_mean;      // ~9.81 when still
        }
    }

    // ---- Gas MEMS (qualitative): log raw mV + Rs so R0 can be derived later ----
    if (present.co) {
        uint32_t mv = readAdcMv(PIN_GAS_CO_ADC);
        doc["co_mv"] = mv;  doc["co_rs"] = gasRs(mv, GAS_CO_RL_OHMS);
    }
    if (present.hcho) {
        uint32_t mv = readAdcMv(PIN_GAS_HCHO_ADC);
        doc["hcho_mv"] = mv;  doc["hcho_rs"] = gasRs(mv, GAS_HCHO_RL_OHMS);
    }

    // ---- Soil moisture ----
    if (present.soil) {
        uint32_t mv = readAdcMv(PIN_SOIL_ADC);
        float pct = 100.0f * (float)(SOIL_DRY_MV - (int)mv) / (float)(SOIL_DRY_MV - SOIL_WET_MV);
        doc["soil_mv"]  = mv;
        doc["soil_pct"] = constrain(pct, 0.0f, 100.0f);
    }

    // ---- Battery (external 1M/2M divider on GPIO32 — log raw always; V/% only if calibrated) ----
    if (present.battery) {
        uint32_t mv = readAdcMv(PIN_BATTERY_ADC);
        doc["bat_raw_mv"] = mv;                 // always trustworthy
        doc["bat_cal"]    = (bool)BAT_CALIBRATED;
        if (BAT_CALIBRATED) {
            float v = mv / 1000.0f * BAT_DIVIDER_FACTOR;
            doc["bat_v"]   = v;
            doc["bat_pct"] = constrain((v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f,
                                       0.0f, 100.0f);
        }
    }

    // ---- INMP441: noise level + per-octave-band dB(A) ----
    if (present.mic) {
        MicResult m;
        if (mic_capture(m)) {
            doc["noise_dba"]  = m.laeq_est;    // uncalibrated A-weighted level
            doc["noise_spl"]  = m.spl_est;
            doc["noise_dbfs"] = m.rms_dbfs;
            doc["noise_clip"] = m.clipping;
            JsonArray bands = doc["noise_bands"].to<JsonArray>();
            for (int b = 0; b < MIC_NBANDS; ++b) bands.add(m.band_dba[b]);
        }
    }

    String out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------------------
// Sync: POST the un-acked tail of the queue in batches, advance cursor on 200.
// Returns true if it made progress (or there was nothing to do).
// ---------------------------------------------------------------------------
static bool syncQueue() {
    if (WiFi.status() != WL_CONNECTED) return false;

    File f = LittleFS.open(QUEUE_PATH, "r");
    if (!f) return true;                 // nothing queued yet
    size_t fileSize = f.size();
    size_t cursor   = cursorRead();
    if (cursor >= fileSize) { f.close(); queueCompactIfDone(); return true; }

    f.seek(cursor);

    // Collect up to a batch of whole lines into a JSON array body.
    String body = "[";
    int n = 0;
    size_t newCursor = cursor;
    while (f.available() && n < SYNC_BATCH_MAX && body.length() < SYNC_BATCH_MAX_BYTES) {
        String line = f.readStringUntil('\n');
        size_t consumed = line.length() + 1;          // +1 for the '\n'
        line.trim();
        if (line.length() == 0) { newCursor += consumed; continue; }
        if (n > 0) body += ",";
        body += line;
        n++;
        newCursor += consumed;
    }
    body += "]";
    f.close();

    if (n == 0) { cursorWrite(newCursor); return true; }

    // Prefer the mDNS-discovered server; fall back to the static SYNC_HOST.
    String   host = s_serverHost.length() ? s_serverHost : String(SYNC_HOST);
    uint16_t port = s_serverHost.length() ? s_serverPort : (uint16_t)SYNC_PORT;

    HTTPClient http;
    String url = String("http://") + host + ":" + String(port) + SYNC_PATH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    int code = http.POST(body);
    http.end();

    if (code == 200 || code == 201 || code == 204) {
        cursorWrite(newCursor);
        Serial.printf("[sync] %d records acked (cursor=%u/%u)\n",
                      n, (unsigned)newCursor, (unsigned)fileSize);
        ledPulse();          // visual confirm: values were sent successfully
        return true;
    }
    Serial.printf("[sync] POST failed code=%d (will retry)\n", code);
    // The server's IP may have changed (new network) — drop the cached address
    // so the next cycle re-discovers it via mDNS.
    s_serverHost = "";
    return false;
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
    Serial.println("\n[boot] air-monitor phase 1");

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

    // First WiFi + time sync + server discovery (all non-fatal if they fail).
    if (wifiConnect()) { syncTimeIfNeeded(); discoverServer(); }

    ledBlink(3);                        // boot done — three quick blinks
    digitalWrite(PIN_EXT_LED, LOW);     // idle: LED off (pulses only on sync)
    Serial.println("[boot] ready — sampling every "
                   + String(SAMPLE_BASELINE_MS / 1000) + " s");
}

void loop() {
    static uint32_t tSample = 0;
    const uint32_t now = millis();

    // --- fixed-cadence baseline sample ---
    if (tSample == 0 || now - tSample >= SAMPLE_BASELINE_MS) {
        tSample = now;

        // Opportunistically (re)connect + set the clock so timestamps are real.
        if (WiFi.status() != WL_CONNECTED) wifiConnect();
        syncTimeIfNeeded();
        // (Re)discover the server if we don't have an address yet this network.
        if (WiFi.status() == WL_CONNECTED && s_serverHost.length() == 0) discoverServer();

        String rec = buildRecord();
        queueAppendLine(rec);
        Serial.printf("[rec] %s\n", rec.c_str());
    }

    // --- drain the sync queue whenever WiFi is up (a few batches per pass) ---
    if (WiFi.status() == WL_CONNECTED) {
        for (int i = 0; i < 5; ++i) {
            if (!syncQueue()) break;          // stop on failure
            if (cursorRead() >= 0) {          // loop guard; syncQueue self-limits
                File f = LittleFS.open(QUEUE_PATH, "r");
                bool caughtUp = !f || cursorRead() >= f.size();
                if (f) f.close();
                if (caughtUp) break;
            }
        }
    }

    delay(50);
}
