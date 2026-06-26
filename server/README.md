# Air Monitor — PC collector + dashboard (Phase 1)

The PC half of the system. Runs on the LattePanda (Debian) in production, but is
pure Python so it runs the same on macOS / Fedora / Windows for development.

## What it does

- **Ingests** records POSTed by the ESP32 (`POST /ingest`)
- **Stores** them in one SQLite file (drop it on a USB stick)
- **Serves** a live dashboard at `http://<pc>:8000/` with history, metric picker,
  time-range query, and event logging
- **Pushes** new records to the browser live over WebSocket
- **Logs home-mode events** (door, ventilation, occupancy, devices, sleep) and
  overlays them on the graphs

## Setup

```bash
cd pc
python3 -m venv .venv && source .venv/bin/activate     # Windows: .venv\Scripts\activate
pip install -r requirements.txt
python server.py
```

Open <http://localhost:8000>. API docs are auto-generated at `/docs`.

### Put the database on a USB stick

```bash
AIRMON_DB=/media/usb/air-monitor.db python server.py      # Linux/macOS
# Windows (PowerShell):  $env:AIRMON_DB="E:\air-monitor.db"; python server.py
```

## Try it without hardware

The simulator posts plausible data (with daily cycles) so you can see the
dashboard work before flashing the ESP32:

```bash
# in a second terminal, with the server running:
python simulate.py --backfill 24      # 24 h of history, then live 1/sec
```

## Running on the LattePanda touch screen (kiosk)

The dashboard is built for the small touch panel: large tap targets, a slide-in
controls drawer (tap **☰**) so charts get the full width, a fullscreen button
(**⛶**), and a screen wake-lock so the browser won't dim while it's showing.

**Auto-start in fullscreen Chromium on boot** (Debian, X11). Create
`~/.config/autostart/air-monitor.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Air Monitor
Exec=chromium --kiosk --app=http://localhost:8000 --start-fullscreen --noerrdialogs --disable-translate
X-GNOME-Autostart-enabled=true
```

(Use `chromium-browser` if that's the binary name on your install.)

**Wake the display on touch.** Letting the screen sleep but waking on a tap is an
OS power setting, not something the page controls. On X11, touch input wakes the
display from DPMS standby by default — just set a blank timeout and make sure DPMS
is on:

```bash
xset s 300         # blank after 5 min idle
xset +dpms         # enable display power management (wakes on input)
xset dpms 0 0 600  # standby/suspend off, power-off after 10 min
```

Put those `xset` lines in `~/.xprofile` (or the autostart) so they apply on login.
If a tap doesn't wake it, your panel may need `xserver-xorg-input-libinput` and a
quick check that the touch device shows up in `xinput list`.

## How the ESP32 finds this server (no static IP needed)

The server advertises itself on the LAN over **mDNS/zeroconf** as
`_airmon._tcp`. The ESP32 looks it up by service name and gets the current
IP + port automatically — so when the station roams to a different network, it
just re-discovers the server. No IP to hardcode or update.

`SYNC_HOST`/`SYNC_PORT` in `firmware/src/secrets.h` are only a **fallback** used if mDNS
discovery turns up nothing (e.g. a network that blocks multicast). You can leave
them at the defaults unless you hit that case.

Verify the advertisement is live:

```bash
# macOS:
dns-sd -B _airmon._tcp
# Linux (avahi-utils):
avahi-browse -r _airmon._tcp
```

> Note: browsing from the *same* machine that runs the server can show nothing
> because the OS mDNS daemon and Python's zeroconf share port 5353 — that's a
> same-host quirk only. The ESP32 (a different host) discovers it fine.

## API quick reference

| Method | Path | Purpose |
|--------|------|---------|
| POST | `/ingest` | Receive one record or an array (from ESP32) |
| GET | `/api/readings?since=&until=&limit=&device=` | Time-range query (epoch seconds) |
| GET | `/api/latest` | Most recent record |
| GET | `/api/metrics` | Numeric fields available to plot |
| GET | `/api/stats` | Count + time span |
| POST | `/api/event` | Log a home-mode event `{kind,label,note,ts?}` |
| GET | `/api/events?since=&until=` | Query events |
| WS | `/ws` | Live push of new readings + events |

## Data model

SQLite, two tables. Sensor fields are stored as a verbatim JSON blob in
`readings.payload` so the schema never needs migrating when sensors change — the
dashboard discovers available metrics by introspection. Indexed columns (`ts`,
`device`) make range queries fast. `pandas.read_sql` reads it directly for later
analysis.

De-duplication is on `(device, ts)`, so the ESP32 re-syncing after a crash is
idempotent — no double-counting.
