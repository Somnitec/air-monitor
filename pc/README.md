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

## Point the ESP32 at this server

In `src/secrets.h` on the firmware side set:

```c
#define SYNC_HOST  "192.168.1.50"   // this PC's LAN IP
#define SYNC_PORT  8000
#define SYNC_PATH  "/ingest"
```

Find the PC's IP: `ip addr` (Linux), `ifconfig` (macOS), `ipconfig` (Windows).

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
