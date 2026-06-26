#pragma once
// Copy this file to  src/secrets.h  (which is gitignored) and fill in real values.
// src/secrets.h is included BEFORE config.h, so these win over the placeholders.

// ---- Wi-Fi ----
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"

// ---- Where to ship data (the LattePanda running pc/server.py) ----
#define SYNC_HOST      "192.168.1.50"   // LattePanda LAN IP
#define SYNC_PORT      8000
#define SYNC_PATH      "/ingest"

// ---- Station location (kept out of the repo on purpose) ----
// Used later for weather + aircraft correlation. Decimal degrees.
#define STATION_LAT    52.179722
#define STATION_LON     5.284722
