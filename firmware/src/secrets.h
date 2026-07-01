#pragma once
// LOCAL secrets — gitignored. Generated from secrets.example.h. Edit with real values.

// ---- Wi-Fi ----
// Primary network — also used by the legacy bring-up test sketches.
#define WIFI_SSID      "Music"
#define WIFI_PASSWORD  "Amsterdam750"

// Full known-network list used by the main firmware (WiFiMulti scans on boot and
// joins the strongest in range). Includes the primary above plus any others.
#define WIFI_NETWORKS { \
    { WIFI_SSID, WIFI_PASSWORD }, \
    { "Nothing", "kleineletters" }, \
    { "Kameraad fazants WiFi service", "krakengaatdoor" }, \
}

// ---- Where to ship data (the LattePanda running pc/server.py) ----
#define SYNC_HOST      "192.168.1.50"
#define SYNC_PORT      8000
#define SYNC_PATH      "/ingest"

// ---- Station location ----
#define STATION_LAT    52.320125
#define STATION_LON     4.878975
