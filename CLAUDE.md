# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is for (the north star)

A DIY air-monitoring station (ESP32 + sensors) next to Schiphol's flight paths. **The
point is the science: find out whether overflying aircraft measurably affect local noise
and air quality** — which planes are loudest, how often/long they fly, whether gas/particle
peaks track flyovers. Everything else (dashboard polish, map eye-candy) serves that goal.

Practical consequences when prioritizing:
- **Data integrity and calibration beat features.** A clean, correctly-timestamped,
  calibrated series is worth more than another UI tweak.
- **Keeping the station alive to gather data matters** — battery gauge must be truthful,
  then power-saving. (The gauge currently lies: it read ~90% minutes before a brownout.)
- The mic is the primary instrument; its calibration (`MIC_DBFS_TO_SPL_DB`) is load-bearing.

## Layout

- `firmware/` — PlatformIO ESP32 firmware (C++). `src/firmware.cpp` is the main app; the
  rest of `src/` + `include/` are the sensor/ring/record modules. `test/` is host-side Unity.
- `server/` — FastAPI + SQLite collector & dashboard (pure Python). `server.py` is the app;
  `aircraft/` and `weather/` are background-poller packages; `station.py` resolves "home".
  `static/index.html` is the **entire** dashboard (one ~2000-line file: HTML+CSS+JS).
- `secrets.h` (repo root, gitignored) and `firmware/src/secrets.h` — real Wi-Fi, server host,
  and `STATION_LAT/LON`. **Real coordinates must never be committed**; `secrets.example.h`
  carries a placeholder only.
- `docs/DEVELOPING.md`, `docs/SENSORS.md`, `DESIGN.md` — deeper notes and the deferred backlog.

## Commands

PlatformIO CLI lives at `~/.platformio/penv/bin/pio`; the server's venv at `server/venv`.

```bash
# --- firmware (run from firmware/) ---
~/.platformio/penv/bin/pio test -e native            # host unit tests (record/ring/seg logic)
~/.platformio/penv/bin/pio run  -e esp32_phase1      # compile/link check (no board needed)
~/.platformio/penv/bin/pio run  -e esp32_phase1 -t upload       # flash over USB
~/.platformio/penv/bin/pio run  -e esp32_phase1_ota -t upload   # flash over Wi-Fi (OTA)

# --- server (run from server/) ---
AIRMON_DB=:memory: ./venv/bin/python -m pytest -q              # full suite (never touches real db)
AIRMON_DB=:memory: ./venv/bin/python -m pytest tests/test_aircraft_base.py::TestX::test_y   # one test
./venv/bin/python server.py                                    # run server + dashboard on :8000
AIRMON_DB=/media/usb/air-monitor.db ./venv/bin/python server.py  # db on a USB stick
python simulate.py --backfill 24                               # fake data, no hardware
```

`esp32_phase1` is the real firmware; `esp32_tester` / `esp32_sensor_test` are bring-up
sketches selected via `build_src_filter`. The `native` test env is Arduino-free (pure logic).

## Architecture — the parts that span files

**Capture → durable queue → sync.** The ESP32 samples every `poll_interval_ms`, packs each
reading into an 84-byte quantized `Record` (`record.cpp`/`record.h`, layout guarded by a
`static_assert`), and appends it to a LittleFS-backed FIFO ring (`ringstore.cpp`) that
survives reboots. It flushes batches to the server `POST /ingest`. NORMAL mode syncs every
~10 s; TESTING mode streams after every sample. The server pushes a `config`/`command` back
in each `/ingest` reply — that's how the dashboard changes `poll_interval_ms` and flips
NORMAL/TESTING (no separate control channel).

**Timestamps are reconstructed server-side (critical).** The ESP32 clock is unreliable, so
device timestamps are often wrong (seen as far-past dates). `server.py` derives an
**effective `ts`** for each reading: trust the device clock only if plausible
(`> EPOCH_VALID_AFTER = 2025-01-01`), otherwise reconstruct from `up_ms` (monotonic uptime)
anchored to `received_at` per boot — preserving intra-batch ordering. Columns: `ts`
(effective, queried), `device_ts` (raw), `ts_source` (`device|uptime|received|corrected`).
If readings "don't show in the graphs," suspect timestamps first.

**Payload is verbatim; columns are generated from it.** `readings`, `weather`, and
`aircraft` keep the full JSON in a `payload` column and expose scalar metrics as SQLite
**generated VIRTUAL columns** (`json_extract(payload,'$.x')`). Migrations that check for an
existing column must use `PRAGMA table_xinfo` — `table_info` omits generated columns (this
caused a real bug). Human-readable views with local-time strings: `readings_h`, `weather_h`,
`aircraft_h`.

**Aircraft sourcing — SDR preferred, internet fallback.** `aircraft/scheduler.poll_once`
reads the local readsb feed (`/run/readsb/aircraft.json`); when present, those are the only
aircraft shown, tagged `source='sdr'`. When the dongle/readsb is unavailable it falls back to
the public airplanes.live feed (`source='public'`). readsb is **not running by default**, so
out of the box every aircraft is `public` (see README "Running readsb").

**Weather/air-quality** is a separate background poller (`weather/`) that writes a sibling
`weather` table, joinable to readings by `weather.valid_ts ≈ readings.ts`. Barometric
pressure and wind come from here — the station has no barometer. Four sources are keyless;
keyed ones self-enable when their `.env` vars exist.

**Research views** (`_create_research_views` in `server.py`) turn the joined tables into the
project's questions — `noise_loud`, `noise_with_aircraft`, `loudness_by_type`,
`daily_summary` — surfaced at `/api/research/*`. They apply **physical-plausibility bounds**
(e.g. noise 0–140 dB) so pre-calibration garbage rows never pollute analysis without deleting
data.

**The dashboard is one static file** served from disk, so edits to `server/static/index.html`
are live on browser reload — **no server restart needed** for dashboard changes (only Python
changes need a restart). Its `render()` draws one Plotly chart per metric **group**, plots
every known metric with a custom legend of chips (value+unit, hover explanation, click to
toggle via `Plotly.restyle`), decimates with min/max bucketing past `MAX_TRACE_POINTS` to
stay fast at wide windows, and overlays a Leaflet aircraft map + a canvas side-view.
Metric→chart grouping and labels live in the `META` / `WEATHER_META` tables at the top.

**Home coordinates** resolve through `station.coords()`: `AIRMON_LAT/LON` env →
`firmware/src/secrets.h` → root `secrets.h` → placeholder. It logs which source won.

## Hardware/firmware gotchas baked into the code

- **I²C runs at 100 kHz, not 400** (`config.h`): the Sensirion SEN66 clock-stretches and
  returns corrupted frames in fast mode (impossible values like RH 404 %, non-monotonic PM).
  SEN66 reads are also gated on `getDataReady()` and a `sen66ValuesSane()` plausibility check.
- **Mic is uncalibrated hardware** calibrated in software: `MIC_DBFS_TO_SPL_DB` (config.h) was
  field-set against a phone SPL meter. Re-check it when the noise study needs absolute dB.
- `secrets.h` is included before `config.h`, so its `#define`s win over the placeholders.

## Working agreements observed in this repo

- Verify changes: `pio test -e native` + `pio run -e esp32_phase1` for firmware; the pytest
  suite for the server. Keep both green.
- WAL-mode SQLite: back up with the online backup API / `.backup` / `VACUUM INTO`, **never a
  plain `cp`** of the live db.
