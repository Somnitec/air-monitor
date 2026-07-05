// Device configuration management (SPIFFS persistence).

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "devconfig.h"
#include "config.h"     // SLOW_INTERVAL_MS — the compiled-in default for the slow channel

#define CONFIG_FILE "/config.json"

// Plausibility bounds so a stray dashboard value can never stall the loop (0 ms) or
// park the station at a uselessly-long cadence. Both intervals are clamped on load
// and on apply.
static const uint32_t POLL_MIN_MS = 1000UL;             // 1 s
static const uint32_t POLL_MAX_MS = 3600UL * 1000UL;    // 1 h
static const uint32_t SLOW_MIN_MS = 5000UL;             // 5 s
static const uint32_t SLOW_MAX_MS = 3600UL * 1000UL;    // 1 h

static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

DeviceConfig devconfig_load() {
    DeviceConfig cfg;
    cfg.poll_interval_ms = 10000;             // default 10 s (override from dashboard)
    cfg.slow_interval_ms = SLOW_INTERVAL_MS;  // default 3 min (compiled-in baseline)

    if (!LittleFS.exists(CONFIG_FILE)) {
        return cfg;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return cfg;

    JsonDocument doc;
    if (!deserializeJson(doc, f)) {
        if (doc["poll_interval_ms"].is<uint32_t>()) {
            cfg.poll_interval_ms = clampU32(doc["poll_interval_ms"], POLL_MIN_MS, POLL_MAX_MS);
        }
        if (doc["slow_interval_ms"].is<uint32_t>()) {
            cfg.slow_interval_ms = clampU32(doc["slow_interval_ms"], SLOW_MIN_MS, SLOW_MAX_MS);
        }
    }
    f.close();
    return cfg;
}

bool devconfig_save(const DeviceConfig& cfg) {
    JsonDocument doc;
    doc["poll_interval_ms"] = cfg.poll_interval_ms;
    doc["slow_interval_ms"] = cfg.slow_interval_ms;

    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) return false;

    bool ok = (serializeJson(doc, f) > 0);
    f.close();
    return ok;
}

bool devconfig_apply_json(const char* json_str, DeviceConfig& cfg) {
    JsonDocument doc;
    if (deserializeJson(doc, json_str)) {
        return false;  // parse error
    }

    DeviceConfig prev = cfg;
    if (doc["poll_interval_ms"].is<uint32_t>()) {
        cfg.poll_interval_ms = clampU32(doc["poll_interval_ms"], POLL_MIN_MS, POLL_MAX_MS);
    }
    if (doc["slow_interval_ms"].is<uint32_t>()) {
        cfg.slow_interval_ms = clampU32(doc["slow_interval_ms"], SLOW_MIN_MS, SLOW_MAX_MS);
    }

    bool changed = (cfg.poll_interval_ms != prev.poll_interval_ms) ||
                   (cfg.slow_interval_ms != prev.slow_interval_ms);
    if (changed) {
        devconfig_save(cfg);
        Serial.printf("[config] intervals updated: poll %u -> %u ms, slow %u -> %u ms\n",
                      prev.poll_interval_ms, cfg.poll_interval_ms,
                      prev.slow_interval_ms, cfg.slow_interval_ms);
    }
    return changed;
}
