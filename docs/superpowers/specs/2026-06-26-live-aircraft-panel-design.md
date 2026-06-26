# Live Aircraft Panel — Design Spec

- **Created:** 2026-06-26
- **Status:** Approved (pending written-spec review)
- **Component of:** DESIGN.md → Mode 2 (Aircraft Pollution Tracker), first increment

## Goal

Show, on the dashboard, **all aircraft currently in the area** around the
station, with a **user-selectable range**, fed by the validated RTL-SDR V4
antenna. Also begin **logging** the aircraft we see to the database, to seed the
later pollution/noise correlation work.

This is the first concrete slice of Mode 2. It deliberately stops at
"see + log live aircraft" — correlation with sensors is a separate feature.

## In scope

- Server reads decoded aircraft from **readsb** and serves them to the dashboard.
- Range math (distance + bearing from home) done server-side.
- Live push to the dashboard over the existing WebSocket.
- Dashboard shows a **map + table** of in-range aircraft with a **range control**.
- **Log** seen aircraft to a new SQLite table (throttled).

## Out of scope (future increments)

- Phase-of-flight inference, Enhanced Mode S / Comm-B fields (IAS, selected
  altitude, roll) — readsb exposes some of these; we may surface them later.
- OpenAP emissions estimates; OpenSky origin/duration/age enrichment.
- Correlation engine (overhead → mic SPL / PM / NOx), overhead-triggered rapid
  sampling.

## Locked decisions

| Decision | Choice |
|---|---|
| Decoder / data source | **readsb** (writes `aircraft.json` with type/registration + bundled hex DB) |
| Display | **Map + table** |
| Storage | **Live view + log to DB** |
| DB log cadence | **1 row per aircraft per ~15 s** while seen (configurable) |
| Panel placement | **Top of the dashboard**, above the sensor charts (collapsible) |
| `aircraft.json` access | **File path** (default), configurable to readsb's HTTP URL |
| Home coordinates | From a **gitignored `.env`**, default = Paleis Soestdijk placeholder |

## Architecture

```
[RTL-SDR V4] --1090MHz--> [readsb] --writes--> aircraft.json
                                                   |
                                   (poll ~1s, read file)
                                                   v
   server/aircraft.py: normalize + distance/bearing from home + drop stale
                                                   |
                        +--------------------------+--------------------------+
                        v                                                     v
              WebSocket broadcast                                  throttled INSERT
              {type:"aircraft", data:[...]}                        into `aircraft` table
                        |
                        v
        Dashboard: filter by range slider -> render map (Leaflet) + table
        Initial load: GET /api/aircraft?range_km=
```

### Components (each isolated and testable)

1. **`server/aircraft.py`** — the only new module with logic.
   - `read_source()` — read + parse readsb `aircraft.json` (file path or URL).
   - `normalize(raw, home)` — for each aircraft **with a position**, produce a
     flat record and compute `distance_km`, `bearing_deg` (haversine) from home.
     Drop entries older than `AIRCRAFT_STALE_SEC` or beyond `AIRCRAFT_MAX_RANGE_KM`.
   - Keeps the current snapshot (dict keyed by `hex`).
   - **Pure where it matters:** `normalize()` takes data in, returns records out —
     unit-testable against a captured `aircraft.json` fixture, no hardware.

2. **server.py wiring (thin):**
   - A background poll task (started in the existing `lifespan`) that every
     `AIRCRAFT_POLL_SEC` reads the source, updates the snapshot, broadcasts it,
     and performs throttled logging.
   - **Logging scope:** the poll task logs **every aircraft in the snapshot**
     (positioned, non-stale, ≤ `AIRCRAFT_MAX_RANGE_KM`), throttled per `hex` to
     `AIRCRAFT_LOG_SEC`. It is **independent of the dashboard's display range** —
     the server doesn't know each client's slider value (the WS is one-way).
   - `GET /api/aircraft?range_km=` — current snapshot sorted by distance;
     `range_km` is **optional** (omitted → all). The dashboard loads it *without*
     a range filter and filters client-side (consistent with the WS path); the
     param exists for external/API callers.
   - Reuses the existing `Hub` (new message `type:"aircraft"`) and `_db_lock`.

3. **DB:** new `aircraft` table + insert helper. Independent of `readings`;
   the charts and existing endpoints are untouched.

4. **Dashboard:** a new **✈ Aircraft** panel + a range control. Existing Plotly
   charts and controls remain as-is.

## Data model

### `aircraft` table

```sql
CREATE TABLE IF NOT EXISTS aircraft (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  ts          INTEGER NOT NULL,      -- epoch seconds when sampled
  hex         TEXT NOT NULL,         -- ICAO 24-bit address
  flight      TEXT,                  -- callsign
  type        TEXT,                  -- ICAO type code (e.g. A320, B738)
  reg         TEXT,                  -- registration
  lat         REAL,
  lon         REAL,
  alt_baro    INTEGER,               -- ft
  gs          REAL,                  -- ground speed, kt
  track       REAL,                  -- deg
  baro_rate   INTEGER,               -- ft/min (+climb / -descent)
  distance_km REAL,                  -- from home
  bearing_deg REAL,                  -- from home
  rssi        REAL
);
CREATE INDEX IF NOT EXISTS idx_aircraft_ts  ON aircraft(ts);
CREATE INDEX IF NOT EXISTS idx_aircraft_hex ON aircraft(hex);
```

### Normalized record (snapshot item + WebSocket payload + `/api/aircraft`)

`{hex, flight, type, reg, lat, lon, alt_baro, gs, track, baro_rate,
distance_km, bearing_deg, rssi, seen}` — `seen` = seconds since last message.

### WebSocket message

`{"type":"aircraft","ts":<epoch>,"data":[<record>, ...]}` — full current
snapshot (all positioned, non-stale aircraft, distance/bearing precomputed)
broadcast each poll tick. The **client filters by the range slider**, so moving
the slider is instant with no round-trip.

## Configuration (PC-side `.env`, gitignored)

| Var | Default | Meaning |
|---|---|---|
| `HOME_LAT` / `HOME_LON` | `52.179722` / `5.284722` (Paleis Soestdijk placeholder) | Station location for range math. Real value goes here, never in a tracked file. |
| `AIRCRAFT_JSON_PATH` | `/run/readsb/aircraft.json` | readsb output file. |
| `AIRCRAFT_JSON_URL` | _(unset)_ | If set, read over HTTP instead of file. |
| `AIRCRAFT_POLL_SEC` | `1` | Poll cadence. |
| `AIRCRAFT_STALE_SEC` | `60` | Drop aircraft not heard from within this. |
| `AIRCRAFT_MAX_RANGE_KM` | `300` | Ignore/skip beyond this (radio horizon sanity). |
| `AIRCRAFT_LOG_SEC` | `15` | Per-aircraft DB log throttle. |

`.gitignore` gets `server/.env`. A `server/.env.example` documents the vars with
the placeholder coordinates.

## Dashboard

- **Panel:** `✈ Aircraft (N in range)` at the top of the page, collapsible.
  Matches the dark/touch theme (CSS variables, ≥44 px targets).
- **Map (Leaflet via CDN):** home marker, **range ring** = slider radius, plane
  markers as `divIcon`s rotated by `track`, colored by altitude band; popup with
  callsign / type / alt / speed / distance.
- **Table:** sortable — callsign · type · altitude · speed · distance · bearing ·
  ↑/↓ (from `baro_rate`); default sort by distance ascending.
- **Range control:** in the controls drawer, a slider (5–250 km) + numeric
  readout, persisted in `localStorage`; drives the ring and the map/table filter.
- **Live:** subscribes to the `aircraft` WS message and re-renders each tick;
  initial fill via `GET /api/aircraft`. Shows "N aircraft in range".

## readsb prerequisite (environmental setup, not code)

readsb must be installed and configured to use the V4 and emit `aircraft.json`
(same device already validated with dump1090). Setup steps will be provided
during implementation (install, `--device-type rtlsdr`, gain, JSON output dir).
Until readsb is running, the server degrades gracefully (see below).

## Error handling

- `aircraft.json` missing / unreadable / stale → poller logs a rate-limited
  warning, serves an empty snapshot; dashboard shows "no aircraft feed".
- Malformed JSON → skip that tick, keep last snapshot briefly, then empty.
- Aircraft without a position → excluded (cannot range-filter or map it).
- Server never crashes because the decoder is down.

## Testing

- **Unit (no hardware):** capture a real `aircraft.json` as a fixture; test
  `normalize()` distance/bearing against a hand-computed value, range filtering,
  stale dropping, and the per-hex log throttle.
- **Manual:** run readsb + server; confirm planes appear on map/table, the range
  slider updates both, and `aircraft` rows accumulate at the throttled cadence.

## Success criteria

- With readsb running, the dashboard shows live aircraft within the chosen range
  on both map and table, updating ~1 Hz.
- Changing the range slider immediately re-filters without a server round-trip.
- The `aircraft` table accumulates ~1 row per aircraft per 15 s.
- With readsb stopped, the dashboard shows "no aircraft feed" and the rest of the
  app (sensor charts, events) works normally.
