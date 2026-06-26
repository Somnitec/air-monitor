# Weather & Air-Quality Integration — Design Spec

**Date:** 2026-06-26
**Status:** Approved, implementing
**Author:** Claude Code (user: Arvid)

## Problem

Stand up a resilient weather/air-quality importer on the PC server that pulls the
full useful set of **current observed** values from the best nearby official +
community sources, stores them as their own time-indexed series in the same SQLite
DB (joinable to sensor readings by timestamp), and backfills gaps when the PC's
intermittent internet returns — so any sensor reading can be correlated against
outdoor conditions, **including pressure and wind that the station has no onboard
sensor for**.

### Assumptions checked
- **No onboard barometer.** SEN66 = PM/RH-T/VOC/NOx/CO₂ only. Pressure comes solely
  from weather imports — this feature fills a real gap rather than duplicating a sensor.
- **Cadence is mismatched on purpose.** Sensors log every 60 s; weather obs are
  10-min (KNMI) to hourly. Weather is stored as its own slower series and joined by
  timestamp, *not* polled in lockstep with readings.
- **Intermittent internet.** The PC may be offline for days; the fetcher must backfill
  on reconnect, not just poll-and-forget.
- **Location:** ~52.179722, 5.284722 (Soestdijk, NL). KNMI De Bilt (06260) is ~8 km away.

### Scope decisions
- **Current/observed values only.** No forecast rows. `kind` is `'obs'`, plus
  `'reanalysis'` used solely to backfill gaps (Open-Meteo ERA5 / CAMS history).
- **Grab everything useful** across all viable sources at each source's native cadence.

## Sources

All behind a uniform `Provider.fetch(since, until) → list[WeatherObs]` interface. A
provider self-disables when its secret is missing or its API errors, never blocking
the others.

| Source | Auth | Cadence | Variables | History |
|---|---|---|---|---|
| Open-Meteo (weather) | keyless | hourly | pressure (MSL+surface), temp, dewpoint, RH, wind speed/gust/dir, precip, cloud, shortwave/direct/diffuse radiation, sunshine, visibility, CAPE, boundary-layer height, ET₀ | ERA5 archive |
| Open-Meteo (air quality) | keyless | hourly | PM2.5, PM10, NO₂, O₃, SO₂, CO, aerosol optical depth, dust, UV, European AQI | CAMS history |
| KNMI De Bilt 06260 (~8 km) | free API key | 10-min | official temp, dewpoint, RH, wind, pressure, precip, sunshine, global radiation, visibility, cloud | limited |
| RIVM Luchtmeetnet | keyless | hourly | official outdoor NO₂/PM10/PM2.5/O₃ | short |
| sensor.community (Luftdaten) | keyless | ~2.5-min | community PM2.5/PM10 (+ BME280 temp/RH/pressure) | forward-only |
| Netatmo | OAuth | ~5–10 min | community PWS pressure/temp/RH/rain/wind | forward-only |
| Weather Underground | API key | varies | community PWS observations | limited |

## Data model

Sibling `weather` table mirroring the `readings` blob pattern (so `pandas.read_sql` and
CSV export generalize) while keeping station provenance:

```sql
CREATE TABLE weather (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    valid_ts    INTEGER NOT NULL,   -- time the obs is FOR (epoch UTC) -- join key
    fetched_at  INTEGER NOT NULL,   -- when we retrieved it
    source      TEXT NOT NULL,      -- 'open_meteo' | 'knmi' | ...
    station_id  TEXT,               -- '06260', PWS id, sensor id, or '' for point/model
    kind        TEXT NOT NULL,      -- 'obs' | 'reanalysis'
    lat REAL, lon REAL, distance_km REAL,
    payload     TEXT NOT NULL,      -- {variable: value, ...} verbatim
    UNIQUE(source, station_id, valid_ts, kind)
);
CREATE INDEX idx_weather_valid  ON weather(valid_ts);
CREATE INDEX idx_weather_source ON weather(source, valid_ts);
```

`UNIQUE(source, station_id, valid_ts, kind)` + `ON CONFLICT … DO UPDATE` makes
re-polling idempotent (a later/better value overwrites), mirroring the existing
`(device, ts)` dedup. Variable→unit map lives in code, exposed at `/api/weather/metrics`.

## Architecture

- **New `weather/` package** beside `server.py`:
  - `base.py` — `WeatherObs` dataclass, `Provider` ABC, `haversine()` distance util.
  - `open_meteo.py`, `knmi.py`, `luchtmeetnet.py`, `sensor_community.py`,
    `netatmo.py`, `wu.py` — one module per source. Each has a **pure
    `parse(raw)→list[WeatherObs]`** (the TDD seam) plus a thin `fetch()` doing HTTP.
  - `store.py` — table DDL + idempotent upsert.
  - `scheduler.py` — the poll loop + gap backfill + resilience.
  - `config.py` — env/`.env` loading; a provider is enabled iff its required vars exist.
- **Background task in the existing FastAPI `lifespan`** (beside the mDNS task). Every
  `WEATHER_POLL_SEC`, fan out to enabled providers with
  `asyncio.gather(..., return_exceptions=True)` so one dead API never blocks the rest;
  each call timeout-bounded with per-provider backoff.
- **Gap backfill:** on startup and after any provider recovers, compare its last
  `valid_ts` to now; if the gap exceeds a threshold, Open-Meteo ERA5 fills weather and
  CAMS fills air-quality history. Sources without history resume forward and **log the
  gap** (no silent holes).
- **HTTP:** `httpx.AsyncClient` (new dep) for non-blocking concurrent fetches.

## Config (PC-side env / `.env`, never committed)

| Var | Default | Purpose |
|---|---|---|
| `AIRMON_LAT`, `AIRMON_LON` | `52.179722`, `5.284722` | our coordinates |
| `WEATHER_RADIUS_KM` | `10` | community station search radius |
| `WEATHER_POLL_SEC` | `600` | poll interval |
| `KNMI_API_KEY` | — | enables KNMI |
| `NETATMO_CLIENT_ID/SECRET/REFRESH_TOKEN` | — | enables Netatmo |
| `WU_API_KEY`, `WU_STATION_ID` | — | enables Weather Underground |

## API

- `GET /api/weather?since=&until=&source=&variable=` — time-range query.
- `GET /api/weather/sources` — active sources + last fetch + station distance.
- `GET /api/weather/metrics` — available variables + units.

Dashboard overlay (outdoor PM next to indoor PM, pressure/wind series) is a **follow-up**;
this spec delivers the joinable data + read API.

## Testing (TDD)

- Each provider's `parse()` turns a recorded fixture response into correct
  `WeatherObs` (right units, distance computed) — no network.
- `store` upsert is idempotent (insert twice → one row; second updates).
- Scheduler gap/backfill logic with an injected clock.
- Resilience: one provider raising does not stop the others.

## Out of scope (now)
- Forecast rows; dashboard overlays; Netatmo/WU if keys unobtainable (providers stay
  dormant without secrets).
