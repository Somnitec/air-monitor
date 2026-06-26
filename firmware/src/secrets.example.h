#pragma once
// Copy this file to  src/secrets.h  (which is gitignored) and fill in real values.
// src/secrets.h is included BEFORE config.h, so these win over the placeholders.

// ---- Wi-Fi ----
// Primary network — also used by the legacy bring-up test sketches.
#define WIFI_SSID      "home-wifi"
#define WIFI_PASSWORD  "home-password"

// Full known-network list used by the main firmware. On boot it scans and joins
// the strongest one in range (ESP32 WiFiMulti), so you can move the station
// between home / workshop / hotspot without reflashing. Add as many as you like.
#define WIFI_NETWORKS { \
    { WIFI_SSID,       WIFI_PASSWORD       }, \
    { "workshop-wifi", "workshop-password" }, \
    { "phone-hotspot", "hotspot-password"  }, \
}

// ---- Where to ship data (the LattePanda running pc/server.py) ----
#define SYNC_HOST      "192.168.1.50"   // LattePanda LAN IP
#define SYNC_PORT      8000
#define SYNC_PATH      "/ingest"

// ---- Station location ----
// Placeholder only (Paleis Soestdijk, Baarn). Put your REAL coordinates in
// secrets.h — this example file is committed, so don't dox yourself here.
// Used later for weather + aircraft correlation. Decimal degrees.
#define STATION_LAT    52.179722
#define STATION_LON     5.284722
