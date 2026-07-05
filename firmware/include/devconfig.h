#pragma once
// Device configuration stored in SPIFFS. Fetched from server after each sync.
// Allows dynamic adjustment of poll interval without reflashing.

#include <stdint.h>

struct DeviceConfig {
    uint32_t poll_interval_ms;  // FAST channel (mic+accel) quiet-store cadence; default 10 s
    uint32_t slow_interval_ms;  // SLOW channel (air quality/gas/soil/battery) re-read; default 3 min
    // Future: gain, noise floor, operating mode, etc.
};

// Load config from /config.json (or use defaults if missing).
DeviceConfig devconfig_load();

// Save config to /config.json.
bool devconfig_save(const DeviceConfig& cfg);

// Apply a config dict from server response (JSON object as string).
// Returns true if config changed.
bool devconfig_apply_json(const char* json_str, DeviceConfig& cfg);
