#pragma once
// Copy this file to  src/secrets.h  (which is gitignored) and fill in real values.
// src/secrets.h is included BEFORE config.h, so these win over the placeholders.

// ---- Wi-Fi (known networks) ----
// List every network the station might see. On boot it scans and joins the
// strongest one that's in range (ESP32 WiFiMulti), so you can move the station
// between home / workshop / hotspot without reflashing. Add as many as you like.
#define WIFI_NETWORKS { \
    { "home-wifi",     "home-password"     }, \
    { "workshop-wifi", "workshop-password" }, \
    { "phone-hotspot", "hotspot-password"  }, \
}

// ---- Where to ship data (the LattePanda running pc/server.py) ----
#define SYNC_HOST      "192.168.1.50"   // LattePanda LAN IP
#define SYNC_PORT      8000
#define SYNC_PATH      "/ingest"

// ---- Station location (kept out of the repo on purpose) ----
// Used later for weather + aircraft correlation. Decimal degrees.
#define STATION_LAT    52.179722
#define STATION_LON     5.284722
