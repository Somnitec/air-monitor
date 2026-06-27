// Device configuration management (SPIFFS persistence).

#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "devconfig.h"

#define CONFIG_FILE "/config.json"

DeviceConfig devconfig_load() {
    DeviceConfig cfg;
    cfg.poll_interval_ms = 60000;  // default 60 s

    if (!LittleFS.exists(CONFIG_FILE)) {
        return cfg;
    }

    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return cfg;

    JsonDocument doc;
    if (!deserializeJson(doc, f)) {
        if (doc["poll_interval_ms"].is<uint32_t>()) {
            cfg.poll_interval_ms = doc["poll_interval_ms"];
        }
    }
    f.close();
    return cfg;
}

bool devconfig_save(const DeviceConfig& cfg) {
    JsonDocument doc;
    doc["poll_interval_ms"] = cfg.poll_interval_ms;

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
        cfg.poll_interval_ms = doc["poll_interval_ms"];
    }

    bool changed = (cfg.poll_interval_ms != prev.poll_interval_ms);
    if (changed) {
        devconfig_save(cfg);
        Serial.printf("[config] poll_interval_ms updated: %u -> %u\n",
                      prev.poll_interval_ms, cfg.poll_interval_ms);
    }
    return changed;
}
