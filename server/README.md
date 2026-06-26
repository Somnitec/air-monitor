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
| GET | `/api/weather?since=&until=&source=&variable=` | External weather/air-quality series |
| GET | `/api/weather/sources` | Active sources, last fetch, nearest station |
| GET | `/api/weather/metrics` | Weather variables available + units |
| WS | `/ws` | Live push of new readings + events + weather |

## Weather & air-quality integration

A background importer pulls **current observed** outdoor conditions from nearby
official + community sources and stores them in a sibling `weather` table, joinable
to sensor readings by timestamp (`weather.valid_ts` ↔ `readings.ts`). This is also
where **barometric pressure and wind** come from — the station has no onboard
barometer.

It polls every `WEATHER_POLL_SEC` (default 10 min), fans out to all enabled providers
concurrently (one dead API never blocks the rest), and **backfills gaps** via
Open-Meteo's ERA5/CAMS history when the PC has been offline.

**Sources** — the four keyless ones run with no setup; the rest self-enable when their
secrets are present in `.env`:

| Source | Auth | Gives |
|--------|------|-------|
| Open-Meteo (weather) | keyless | pressure, wind, temp, humidity, precip, cloud, radiation, visibility, boundary-layer height |
| Open-Meteo (air quality) | keyless | PM2.5/PM10, NO₂, O₃, SO₂, CO, AOD, dust, UV, European AQI |
| RIVM Luchtmeetnet | keyless | official outdoor NO₂/PM/O₃ (sensor validation) |
| sensor.community | keyless | dense community PM2.5/PM10 |
| KNMI De Bilt 06260 | free API key | official 10-min station obs |
| Netatmo | OAuth | community PWS pressure/temp/RH/wind |
| Weather Underground | API key | community PWS observations |

The four keyless sources work out of the box — nothing to configure.

### Adding the keyed sources (KNMI / Netatmo / WU)

These three need credentials. They stay **dormant** until their vars are set, so the
server runs fine without them; add whichever you've collected.

1. **Get the keys:**
   - **KNMI** — a free Open Data API key from <https://developer.dataplatform.knmi.nl>
     (just a token, not OAuth). → `KNMI_API_KEY`
   - **Netatmo** — create an app at <https://dev.netatmo.com> (scope `read_station`),
     then do the one-time OAuth to mint a refresh token. → `NETATMO_CLIENT_ID`,
     `NETATMO_CLIENT_SECRET`, `NETATMO_REFRESH_TOKEN`
   - **Weather Underground** — an API key + a station id to query.
     → `WU_API_KEY`, `WU_STATION_ID`. ⚠️ WU's free PWS key generally requires you to
     *operate* a PWS that uploads data; if you don't, this one may be unobtainable.

2. **Put them in `server/.env`** (gitignored — copy the template):

   ```bash
   cp .env.example .env      # then fill in the keyed sources you have
   ```

3. **Smoke-test them live** (hits each configured provider, writes nothing to the DB):

   ```bash
   python verify_weather_keys.py
   ```

   Result legend: `✓` working (prints a sample row) · `⚠` connected but 0 obs
   (usually a wrong station id or too-tight `WEATHER_RADIUS_KM`) · `✗` error (prints
   it) · `•` not configured. Once a provider shows `✓`, the running server picks it up
   on its next poll — no restart needed beyond reloading `.env`.

**Likely snags per provider** (these parsers are unit-tested against each API's
documented shape but had no live key when written, so confirm with the smoke test):

- **KNMI** — the unverified part is its EDR collection slug + parameter codes. A `✗`
  404 or empty `ranges` means the collection name or `_CODE_MAP` in
  [`weather/knmi.py`](weather/knmi.py) needs adjusting to what your key actually returns.
- **Netatmo** — the refresh token rotates on each use; the provider keeps the rotated
  one in memory for the process. The first OAuth mint is the fiddly step.
- **WU** — a `•`/`✗` here is often the PWS-ownership requirement above, not a bug.

If the smoke test prints a `✗`/`⚠`, paste the output to whoever's maintaining the
weather code (or open an issue) — fixing a parser is a few-line change against the
real response.

Design rationale lives in
[`docs/superpowers/specs/2026-06-26-weather-integration-design.md`](../docs/superpowers/specs/2026-06-26-weather-integration-design.md).

## Data model

SQLite, two tables. Sensor fields are stored as a verbatim JSON blob in
`readings.payload` so the schema never needs migrating when sensors change — the
dashboard discovers available metrics by introspection. Indexed columns (`ts`,
`device`) make range queries fast. `pandas.read_sql` reads it directly for later
analysis.

De-duplication is on `(device, ts)`, so the ESP32 re-syncing after a crash is
idempotent — no double-counting.
