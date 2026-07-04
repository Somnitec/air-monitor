#!/usr/bin/env python3
"""
Air Monitor — PC-side collector + dashboard (Phase 1).

One FastAPI app that:
  - ingests records POSTed by the ESP32            (POST /ingest)
  - stores them in a single SQLite file            (USB-stick friendly)
  - serves a live dashboard with history + query   (GET /)
  - pushes new records to the browser live         (WS  /ws)
  - lets you mark home-mode events                 (POST /api/event)

Runs identically on macOS, Fedora, Windows and the Debian LattePanda — pure
Python, no native build step.

    pip install -r requirements.txt
    python server.py                 # or: uvicorn server:app --host 0.0.0.0 --port 8000

Database location (default ./data/air-monitor.db) is overridable:
    AIRMON_DB=/media/usb/air-monitor.db python server.py
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import math
import os
import socket
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Form, Request, WebSocket
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.middleware.gzip import GZipMiddleware
from starlette.responses import Response

import station

from weather import config as weather_config
from weather import scheduler as weather_scheduler
from weather import store as weather_store

from aircraft import config as aircraft_config
from aircraft import scheduler as aircraft_scheduler
from aircraft import store as aircraft_store
from aircraft import usb as aircraft_usb

# Load .env before reading any env vars so CLOUDFLARED_TUNNEL (and API keys) are
# available at module level. A second load inside lifespan is harmless (setdefault).
weather_config.load_dotenv(Path(__file__).parent / ".env")

# --------------------------------------------------------------------------- #
# Storage
# --------------------------------------------------------------------------- #
DB_PATH = Path(os.environ.get("AIRMON_DB", Path(__file__).parent / "data" / "air-monitor.db"))
DB_PATH.parent.mkdir(parents=True, exist_ok=True)

_db_lock = asyncio.Lock()
_conn: sqlite3.Connection

# Dashboard/analysis reads run on a second, read-only connection inside a worker
# thread: every _conn.execute() is synchronous on the event loop, so a slow scan
# there freezes /ingest, websockets — everything — for its whole duration. WAL mode
# gives readers a consistent snapshot concurrent with the writer.
_ro_conn: sqlite3.Connection | None = None
_ro_lock = asyncio.Lock()


def _open_ro_conn() -> sqlite3.Connection | None:
    """Read-only companion connection to DB_PATH. Returns None for in-memory DBs
    (tests): a second ':memory:' connection would be a different, empty database,
    so those fall back to the shared connection under the write lock."""
    if str(DB_PATH) == ":memory:":
        return None
    conn = sqlite3.connect(f"{DB_PATH.resolve().as_uri()}?mode=ro",
                           uri=True, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA busy_timeout=5000;")
    return conn


async def _read_query(fn):
    """Run `fn(conn)` — a blocking, read-only DB function — without stalling the
    event loop. `_ro_lock` serialises access: one worker thread at a time may use
    the sqlite connection."""
    if _ro_conn is not None:
        async with _ro_lock:
            return await asyncio.to_thread(fn, _ro_conn)
    async with _db_lock:
        return fn(_conn)


# The research aggregates scan the whole history, so a dashboard (or several open
# tabs) refreshing every couple of minutes must not re-run them back to back.
# Expired entries are pruned on every miss, keeping the cache bounded.
_research_cache: dict[str, tuple[float, Any]] = {}
_RESEARCH_TTL = 60.0


async def _cached_read(key: str, fn):
    now = time.monotonic()
    hit = _research_cache.get(key)
    if hit is not None and now - hit[0] < _RESEARCH_TTL:
        return hit[1]
    result = await _read_query(fn)
    for k in [k for k, (t, _) in _research_cache.items() if now - t >= _RESEARCH_TTL]:
        del _research_cache[k]
    _research_cache[key] = (now, result)
    return result


# Records timestamped before this are from an unsynced ESP32 clock and can't be
# trusted (mirrors firmware EPOCH_VALID_AFTER = 2025-01-01).
EPOCH_VALID_AFTER = 1735689600

# Numeric scalar fields unpacked from the JSON payload into real, queryable columns.
# Generated VIRTUAL columns: no value duplication, payload stays the source of truth,
# but `SELECT pm25, noise_dba ...` and indexes Just Work for analysis. Per-band arrays
# (noise_bands, accel bands) stay in the payload — query them with json_extract as needed.
READING_METRIC_COLS = [
    "pm1", "pm25", "pm4", "pm10", "co2", "voc", "nox", "temp", "rh", "lux",
    "pressure_hpa", "bme_temp", "bme_rh",
    "rumble", "rumble_peak", "accel_mag", "ppv_mm_s", "accel_dom_hz",
    "co_mv", "co_rs", "hcho_mv", "hcho_rs", "soil_mv", "soil_pct",
    "bat_raw_mv", "bat_v", "bat_pct",
    "noise_dba", "noise_spl", "noise_dbfs", "lamax", "lceq", "lc_minus_la",
]


# Slow-channel keys the firmware delta-encodes (schema v3): when a slow sensor isn't
# re-read, the device omits these keys and we carry the last value forward; an explicit
# null means the sensor read failed (a real gap). Mirrors the FS_* groups the firmware
# marks FS_UNCHANGED (record_to_json / readSlow). Fast-channel keys (noise_*, vib_*,
# rumble*, ppv*) are NOT here — they're read every record, never carried.
SLOW_FILL_KEYS = [
    "pm1", "pm25", "pm4", "pm10", "co2", "voc", "nox", "temp", "rh",  # SEN66
    "lux",                                                            # BH1750
    "pressure_hpa", "bme_temp", "bme_rh",                             # BME280
    "co_mv", "co_rs", "hcho_mv", "hcho_rs",                           # gas
    "soil_mv", "soil_pct",                                            # soil
    "bat_raw_mv", "bat_cal", "bat_v", "bat_pct",                      # battery
]


def _metric_col_ddl() -> str:
    return ",\n            ".join(
        f"{c} REAL GENERATED ALWAYS AS (json_extract(payload,'$.{c}')) VIRTUAL"
        for c in READING_METRIC_COLS
    )


def _init_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")     # crash-resilient, good for a USB stick
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.executescript(
        f"""
        CREATE TABLE IF NOT EXISTS readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          INTEGER NOT NULL,          -- effective unix epoch (best estimate, queryable)
            device_ts   INTEGER,                   -- raw ts the device reported (may be unsynced/garbage)
            device      TEXT,
            up_ms       INTEGER,                   -- device uptime at capture (monotonic, always reliable)
            boot        INTEGER,                   -- per-boot id (up_ms resets each boot)
            ts_ok       INTEGER DEFAULT 1,         -- 0 = ESP32 clock was unsynced at capture
            ts_source   TEXT DEFAULT 'device',     -- 'device' | 'uptime' | 'received' | 'corrected'
            received_at INTEGER NOT NULL,          -- server unix epoch when stored
            payload     TEXT NOT NULL,             -- full JSON record, verbatim
            {_metric_col_ddl()}
        );
        CREATE INDEX IF NOT EXISTS idx_readings_ts     ON readings(ts);
        CREATE INDEX IF NOT EXISTS idx_readings_device ON readings(device);
        CREATE INDEX IF NOT EXISTS idx_readings_dedup  ON readings(device, device_ts, boot);
        -- _backfill_times runs on nearly every ingest batch; without this it scans
        -- every row of the device (grows with the table) instead of one boot's slice.
        CREATE INDEX IF NOT EXISTS idx_readings_boot   ON readings(device, boot, ts_source);
        -- Covers the research views' reading-side scans. The metric columns are
        -- generated VIRTUAL (json_extract on access), so without this every
        -- aggregate reads every row's full JSON payload — the whole table.
        CREATE INDEX IF NOT EXISTS idx_readings_research
            ON readings(ts, lamax, noise_dba, pm25, co2);

        CREATE TABLE IF NOT EXISTS events (
            id    INTEGER PRIMARY KEY AUTOINCREMENT,
            ts    INTEGER NOT NULL,                -- when the event happened (epoch)
            kind  TEXT NOT NULL,                   -- 'door','ventilation','occupancy','device','sleep',...
            label TEXT,                            -- e.g. 'open','on','PC','heating'
            note  TEXT
        );
        CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts);

        -- Human-readable view: timestamps as local-time strings + the unpacked metrics.
        DROP VIEW IF EXISTS readings_h;
        CREATE VIEW readings_h AS
            SELECT id,
                   datetime(ts, 'unixepoch', 'localtime')          AS time,
                   datetime(received_at, 'unixepoch', 'localtime') AS received,
                   ts_source, device, boot,
                   {', '.join(READING_METRIC_COLS)}
            FROM readings ORDER BY ts;
        """
    )
    _migrate_readings(conn)
    conn.commit()
    weather_store.init_weather_table(conn)    # external weather/air-quality series
    aircraft_store.init_aircraft_table(conn)  # ADS-B sightings (logged, throttled)
    _create_research_views(conn)              # analysis views over the joined data
    return conn


def _create_research_views(conn: sqlite3.Connection) -> None:
    """Read-only views that turn the raw tables into the project's research questions:
    how loud, how often, which aircraft, day vs night, plane-noise correlation. They
    join readings↔aircraft by time proximity, so they only fill in once the mic is
    reporting noise_dba/lamax. Cheap to (re)define; nothing computes until queried."""
    # Physically-plausible bounds so corrupted frames (old garbage like 222 dB / RH
    # 404 % / PM 5000) never pollute the analysis — non-destructive (rows stay in the
    # table; the views just ignore the impossible ones).
    conn.executescript(
        """
        -- Every reading at/above 65 dB(A), tagged with hour + night flag (23:00–07:00).
        DROP VIEW IF EXISTS noise_loud;
        CREATE VIEW noise_loud AS
        SELECT id, ts,
               datetime(ts,'unixepoch','localtime') AS time,
               noise_dba, lamax, lceq,
               COALESCE(lamax, noise_dba) AS peak_db,
               CAST(strftime('%H', ts, 'unixepoch', 'localtime') AS INTEGER) AS hour,
               (CAST(strftime('%H', ts,'unixepoch','localtime') AS INTEGER) < 7 OR
                CAST(strftime('%H', ts,'unixepoch','localtime') AS INTEGER) >= 23) AS night
        FROM readings
        WHERE COALESCE(lamax, noise_dba) BETWEEN 65 AND 140
        ORDER BY ts;

        -- Each loud-ish reading joined to the closest aircraft overhead within ±30 s.
        -- "Which plane made this noise?" — the basis for cause attribution.
        -- The ±30 s window is a BETWEEN (not ABS()) so the aircraft ts index is used;
        -- ABS() forced a full aircraft scan per loud reading (seconds, growing daily).
        DROP VIEW IF EXISTS noise_with_aircraft;
        CREATE VIEW noise_with_aircraft AS
        SELECT r.ts,
               datetime(r.ts,'unixepoch','localtime') AS time,
               r.noise_dba, r.lamax,
               a.hex, a.flight, a.type, a.category, a.operator,
               a.alt_baro, a.gs, a.baro_rate,
               round(a.distance_km, 2) AS dist_km
        FROM readings r INDEXED BY idx_readings_research
        LEFT JOIN aircraft a ON a.id = (
            SELECT a2.id FROM aircraft a2
            WHERE a2.ts BETWEEN r.ts - 30 AND r.ts + 30
            ORDER BY a2.distance_km ASC LIMIT 1)
        WHERE COALESCE(r.lamax, r.noise_dba) BETWEEN 60 AND 140
        ORDER BY r.ts;

        -- Which aircraft TYPES are loudest (avg + peak), for types seen overhead (≤8 km).
        -- Driven from readings (the smaller table) with a BETWEEN range join so each
        -- reading probes the aircraft ts index; the old ABS() join compared every
        -- reading against every sighting and stopped finishing after a week of data.
        DROP VIEW IF EXISTS loudness_by_type;
        CREATE VIEW loudness_by_type AS
        SELECT a.type,
               COUNT(*) AS n_samples,
               round(AVG(r.noise_dba), 1) AS avg_dba,
               round(MAX(COALESCE(r.lamax, r.noise_dba)), 1) AS max_dba
        FROM readings r INDEXED BY idx_readings_research
        JOIN aircraft a ON a.ts BETWEEN r.ts - 15 AND r.ts + 15
        WHERE a.distance_km <= 8 AND a.type IS NOT NULL
          AND r.noise_dba BETWEEN 0 AND 140
        GROUP BY a.type
        HAVING n_samples >= 3
        ORDER BY avg_dba DESC;

        -- Per local day: flights overhead, loudness, event counts, air quality.
        -- "Clearly better air when they don't fly for a day?" lives here.
        -- One GROUP BY pass over each table; the previous version ran eight
        -- correlated full-scan subqueries per day (O(days × rows)).
        DROP VIEW IF EXISTS daily_summary;
        CREATE VIEW daily_summary AS
        -- INDEXED BY: the metric columns are generated VIRTUAL, and the planner
        -- keeps choosing a table scan (reading every row's whole JSON payload)
        -- over the much smaller covering index that stores their values.
        WITH rd AS (
            SELECT date(ts,'unixepoch','localtime') AS day,
                   round(MAX(CASE WHEN COALESCE(lamax,noise_dba) BETWEEN 0 AND 140
                                  THEN COALESCE(lamax,noise_dba) END),1) AS max_db,
                   COUNT(CASE WHEN COALESCE(lamax,noise_dba) BETWEEN 65 AND 140
                              THEN 1 END) AS n_ge65,
                   COUNT(CASE WHEN COALESCE(lamax,noise_dba) BETWEEN 70 AND 140
                              THEN 1 END) AS n_ge70,
                   COUNT(CASE WHEN COALESCE(lamax,noise_dba) BETWEEN 65 AND 140
                              AND (CAST(strftime('%H',ts,'unixepoch','localtime') AS INTEGER)<7
                                OR CAST(strftime('%H',ts,'unixepoch','localtime') AS INTEGER)>=23)
                              THEN 1 END) AS n_ge65_night,
                   round(AVG(CASE WHEN pm25 BETWEEN 0 AND 1000 THEN pm25 END),1) AS avg_pm25,
                   round(AVG(CASE WHEN co2 BETWEEN 0 AND 40000 THEN co2 END),0) AS avg_co2
            FROM readings INDEXED BY idx_readings_research GROUP BY day),
        ac AS (
            SELECT date(ts,'unixepoch','localtime') AS day,
                   COUNT(DISTINCT hex) AS flights
            FROM aircraft WHERE distance_km <= 10 GROUP BY day),
        days AS (SELECT day FROM rd UNION SELECT day FROM ac)
        SELECT days.day,
               COALESCE(ac.flights, 0) AS flights,
               rd.max_db, rd.n_ge65, rd.n_ge70, rd.n_ge65_night,
               rd.avg_pm25, rd.avg_co2
        FROM days
        LEFT JOIN rd ON rd.day = days.day
        LEFT JOIN ac ON ac.day = days.day
        ORDER BY days.day DESC;
        """
    )
    conn.commit()


def _migrate_readings(conn: sqlite3.Connection) -> None:
    """Bring a pre-existing readings table up to the current schema (add the new
    bookkeeping + generated metric columns) and fix up implausible timestamps."""
    # table_xinfo (not table_info) also lists generated columns, so we don't try to
    # re-add an existing virtual column.
    cols = {r[1] for r in conn.execute("PRAGMA table_xinfo(readings)")}
    add = []
    if "device_ts" not in cols:
        add.append("ALTER TABLE readings ADD COLUMN device_ts INTEGER")
    if "up_ms" not in cols:
        add.append("ALTER TABLE readings ADD COLUMN up_ms INTEGER")
    if "boot" not in cols:
        add.append("ALTER TABLE readings ADD COLUMN boot INTEGER")
    if "ts_source" not in cols:
        add.append("ALTER TABLE readings ADD COLUMN ts_source TEXT DEFAULT 'device'")
    for stmt in add:
        conn.execute(stmt)
    for c in READING_METRIC_COLS:
        if c not in cols:
            conn.execute(
                f"ALTER TABLE readings ADD COLUMN {c} REAL "
                f"GENERATED ALWAYS AS (json_extract(payload,'$.{c}')) VIRTUAL"
            )
    if add:
        # Backfill new bookkeeping columns from the stored payload.
        conn.execute(
            "UPDATE readings SET "
            "  device_ts = COALESCE(device_ts, ts), "
            "  up_ms = COALESCE(up_ms, json_extract(payload,'$.up_ms')), "
            "  boot  = COALESCE(boot,  json_extract(payload,'$.boot'))"
        )
        # One-time repair: any row whose effective ts is implausibly old (unsynced
        # device clock) gets re-derived from uptime, anchored to the newest record of
        # its boot (whose up_ms ≈ received_at). Keeps intra-boot ordering, lands it in
        # real time so it shows up in the dashboard.
        conn.execute(
            f"""
            WITH anchor AS (
                SELECT boot,
                       MAX(up_ms) AS max_up,
                       MAX(received_at) AS recv
                FROM readings WHERE boot IS NOT NULL AND up_ms IS NOT NULL
                GROUP BY boot
            )
            UPDATE readings
               SET ts = CAST(a.recv - (a.max_up - readings.up_ms) / 1000.0 AS INTEGER),
                   ts_source = 'uptime'
              FROM anchor a
             WHERE readings.boot = a.boot
               AND readings.up_ms IS NOT NULL
               AND readings.ts < {EPOCH_VALID_AFTER}
            """
        )


def _effective_ts(rec: dict[str, Any], received_at: int, boot_anchor_up_ms: int | None) -> tuple[int, str]:
    """Return (effective_ts, source). Trust the device clock when it's plausibly
    synced; otherwise reconstruct from uptime: the record with the largest up_ms in
    this boot's batch was captured ≈ received_at, so every record's real time is
    received_at − (max_up_ms − this_up_ms). Falls back to received_at if no uptime."""
    device_ts = int(rec.get("ts", 0) or 0)
    ts_ok = bool(rec.get("ts_ok", True))
    now = received_at
    if ts_ok and EPOCH_VALID_AFTER < device_ts <= now + 86400:
        return device_ts, "device"
    up_ms = rec.get("up_ms")
    if up_ms is not None and boot_anchor_up_ms is not None:
        return int(received_at - (boot_anchor_up_ms - up_ms) / 1000.0), "uptime"
    return received_at, "received"



def _dedupe_insert(rec: dict[str, Any], received_at: int, eff_ts: int, source: str) -> bool:
    """Insert one reading at its effective timestamp. Returns True if newly stored,
    False if a duplicate (same device + device_ts + boot) was skipped — makes
    re-syncs after a crash idempotent."""
    device = rec.get("dev") or rec.get("device")
    device_ts = int(rec.get("ts", 0) or 0)
    boot = rec.get("boot")
    ts_ok = 1 if rec.get("ts_ok", True) else 0

    cur = _conn.execute(
        "SELECT 1 FROM readings WHERE device IS ? AND device_ts=? AND boot IS ? LIMIT 1",
        (device, device_ts, boot),
    )
    if cur.fetchone() is not None:
        return False
    _conn.execute(
        "INSERT INTO readings "
        "(ts, device_ts, device, up_ms, boot, ts_ok, ts_source, received_at, payload) "
        "VALUES (?,?,?,?,?,?,?,?,?)",
        (eff_ts, device_ts, device, rec.get("up_ms"), boot, ts_ok, source, received_at,
         json.dumps(rec, separators=(",", ":"))),
    )
    return True


def _backfill_times(device: Any, boot: Any) -> int:
    """Promote a boot's uptime-derived estimates to true time once a record with a
    real synced clock (ts_source='device') arrives. boot_epoch = device_ts − up_ms/1000
    from that record, so every other record's true time is boot_epoch + up_ms/1000.
    Only db columns change; payload stays verbatim. Returns the rows corrected."""
    if boot is None:
        return 0
    row = _conn.execute(
        "SELECT device_ts - up_ms/1000.0 FROM readings "
        "WHERE device IS ? AND boot=? AND ts_source='device' AND up_ms IS NOT NULL "
        "ORDER BY ts LIMIT 1",
        (device, boot),
    ).fetchone()
    if row is None or row[0] is None:
        return 0
    boot_epoch = row[0]
    cur = _conn.execute(
        "UPDATE readings "
        "SET ts = CAST(? + up_ms/1000.0 AS INTEGER), ts_source='corrected' "
        "WHERE device IS ? AND boot=? AND up_ms IS NOT NULL "
        "AND ts_source IN ('uptime','received')",
        (boot_epoch, device, boot),
    )
    return cur.rowcount


# --------------------------------------------------------------------------- #
# Live WebSocket fan-out
# --------------------------------------------------------------------------- #
class Hub:
    def __init__(self) -> None:
        self.clients: set[WebSocket] = set()

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()
        self.clients.add(ws)

    def disconnect(self, ws: WebSocket) -> None:
        self.clients.discard(ws)

    async def broadcast(self, msg: dict) -> None:
        # Broadcast is awaited inside /ingest and the 1 Hz aircraft loop, so one
        # stalled client (backpressured send) must not freeze the whole server:
        # a client that can't take the message within 1 s is dropped.
        dead = []
        for ws in list(self.clients):
            try:
                await asyncio.wait_for(ws.send_json(msg), timeout=1.0)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)


hub = Hub()


# --------------------------------------------------------------------------- #
# Per-device live state + a one-shot command the dashboard can queue for a
# device. The device reports its mode/buffered count on every /ingest; we return
# any pending command in the reply and clear it once the device echoes the mode.
# --------------------------------------------------------------------------- #
DEVICE_STATE: dict[str, dict] = {}     # dev -> {mode,last_seen,buffered,boot,fw}
PENDING_CMD: dict[str, str] = {}       # dev -> "testing" | "normal"

# Delta-sync forward-fill (firmware Phase 3). The device sends slow-channel values
# only when freshly read; in between it OMITS those keys (carry the last value
# forward) and on a failed read it sends an explicit null (a real gap — keep it).
# We reconstruct full payloads here so the generated metric columns stay populated.
# Cache is per-device and per-process: after a server restart it's cold, so a few
# minutes of slow-channel NULLs are expected until the next fresh read repopulates it.
SLOW_FILL_KEYS: tuple[str, ...] = (
    "pm1", "pm25", "pm4", "pm10", "co2", "voc", "nox", "temp", "rh",   # SEN66
    "lux",                                                             # BH1750
    "pressure_hpa", "bme_temp", "bme_rh",                              # BME280
    "co_mv", "co_rs", "hcho_mv", "hcho_rs",                            # gas
    "soil_mv", "soil_pct",                                             # soil
    "bat_raw_mv", "bat_cal", "bat_v", "bat_pct",                       # battery
)
_LAST_VALUES: dict[str, dict[str, Any]] = {}   # dev -> {key: last good value}


def _seed_fill_cache(dev: Any) -> dict[str, Any]:
    """Warm a device's forward-fill cache from its most recent stored reading. The
    cache is per-process, so a server restart (or moving to a fresh DB) starts it cold
    and the first delta-encoded records — the ones that OMIT unchanged slow values —
    would land with NULL PM/CO2/temp/… until the next fresh read minutes later. Seeding
    from the last row (whose payload was already forward-filled before storage) closes
    that gap. Returns the seeded cache (also stored in _LAST_VALUES)."""
    cache: dict[str, Any] = {}
    try:
        row = _conn.execute(
            "SELECT payload FROM readings WHERE device IS ? ORDER BY ts DESC LIMIT 1",
            (dev,),
        ).fetchone()
        if row:
            payload = json.loads(row["payload"])
            for k in SLOW_FILL_KEYS:
                if payload.get(k) is not None:
                    cache[k] = payload[k]
    except Exception:
        pass
    _LAST_VALUES[dev] = cache
    return cache


def _forward_fill(records: list, default_dev: Any) -> None:
    """Reconstruct delta-encoded slow fields across a batch, in place and in order.
    Per slow key of each record: present & non-null -> a fresh value (remember it);
    present & null -> an explicit gap from a failed read (leave null, keep the last
    good value cached); absent -> carried forward from cache if we've ever seen it
    for that device (else left absent -> column NULL). Records carry their own `dev`;
    `default_dev` covers bare records from the envelope."""
    for rec in records:
        if not isinstance(rec, dict):
            continue
        dev = rec.get("dev") or rec.get("device") or default_dev
        if dev is None:
            continue
        cache = _LAST_VALUES.get(dev)
        if cache is None:                    # first sight this process — warm from DB
            cache = _seed_fill_cache(dev)
        for k in SLOW_FILL_KEYS:
            if k in rec:
                if rec[k] is not None:
                    cache[k] = rec[k]      # fresh read — update the carry-forward value
                # else: explicit null (FS_INVALID) — leave as a gap, don't touch cache
            elif k in cache:
                rec[k] = cache[k]          # omitted (FS_UNCHANGED) — carry last value forward


def _update_device(dev: str, mode: str, buffered, boot, fw) -> None:
    DEVICE_STATE[dev] = {
        "dev": dev, "mode": mode, "buffered": buffered,
        "boot": boot, "fw": fw, "last_seen": int(time.time()),
    }
    # Clear a pending command once the device reports the target mode.
    if PENDING_CMD.get(dev) == mode:
        PENDING_CMD.pop(dev, None)


# --------------------------------------------------------------------------- #
# mDNS / zeroconf advertisement — lets the ESP32 find us by service name on
# whatever network we're on, instead of a hardcoded IP. Degrades gracefully if
# the zeroconf package isn't installed.
# --------------------------------------------------------------------------- #
PORT = int(os.environ.get("PORT", 8000))


def _enumerate_ipv4() -> list[str]:
    """Every non-loopback IPv4 address on this host, any interface. Uses `ifaddr`
    (a transitive dep of zeroconf), so it needs no route to the internet — the key
    to working on an offline hotspot where the 8.8.8.8 trick below fails."""
    ips: list[str] = []
    try:
        import ifaddr
        for adapter in ifaddr.get_adapters():
            for ip in adapter.ips:
                addr = ip.ip
                if isinstance(addr, str) and "." in addr and not addr.startswith("127."):
                    if addr not in ips:
                        ips.append(addr)
    except Exception:
        pass
    return ips


def _local_ips() -> list[str]:
    """LAN IPv4 addresses other devices can reach us on, most-likely-primary first.
    Works offline: with no default route (a hotspot with no upstream) the 8.8.8.8
    route lookup raises, so we fall back to enumerating every interface address. The
    old code returned 127.0.0.1 here, which then got advertised over mDNS — so the
    ESP32 'found' the server at its own loopback and could never deliver anything."""
    ordered: list[str] = []
    # 1. If a default route exists, its source IP is the primary LAN address.
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # no packets sent; just resolves the route's source IP
        ip = s.getsockname()[0]
        if not ip.startswith("127."):
            ordered.append(ip)
    except OSError:
        pass
    finally:
        s.close()
    # 2. Add every other interface address (the no-route case + multi-homed hosts,
    #    e.g. ethernet-with-internet AND a hotspot at the same time).
    for ip in _enumerate_ipv4():
        if ip not in ordered:
            ordered.append(ip)
    return ordered or ["127.0.0.1"]


def _local_ip() -> str:
    """Best-effort primary LAN IP (the address other devices reach us on)."""
    return _local_ips()[0]


async def _start_mdns():
    """Register the _airmon._tcp service. Uses AsyncZeroconf because we're inside
    a running asyncio loop (the sync Zeroconf API deadlocks here)."""
    try:
        from zeroconf import ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        print("[mdns] zeroconf not installed — ESP32 must use the static SYNC_HOST fallback")
        return None, None
    ips = _local_ips()
    # Use a fixed hostname (not socket.gethostname(), which the ESP32 can't predict)
    # so the device can resolve our A-record directly via MDNS.queryHost("airmon-server").
    # zeroconf registers airmon-server.local. -> ip(s) for us. Advertise ALL interface
    # addresses (primary first) so a device on any of them — the hotspot subnet included
    # — gets a reachable one; the ESP32 also has a gateway fallback for laptop-as-hotspot.
    info = ServiceInfo(
        "_airmon._tcp.local.",
        "air-monitor._airmon._tcp.local.",
        addresses=[socket.inet_aton(ip) for ip in ips],
        port=PORT,
        properties={"path": "/ingest"},
        server="airmon-server.local.",
    )
    aiozc = AsyncZeroconf()
    try:
        await aiozc.async_register_service(info)
    except Exception as exc:
        # NonUniqueNameException on rapid restart (or in tests); mDNS is best-effort.
        await aiozc.async_close()
        print(f"[mdns] could not register service ({exc.__class__.__name__}) — "
              "ESP32 must use SYNC_HOST fallback")
        return None, None
    print(f"[mdns] advertising _airmon._tcp at {', '.join(ips)}:{PORT}")
    return aiozc, info


# --------------------------------------------------------------------------- #
# Optional Cloudflare Tunnel — start it alongside the server so the dashboard is
# reachable remotely without port-forwarding. Opt in by setting CLOUDFLARED_TUNNEL
# to your tunnel name (e.g. CLOUDFLARED_TUNNEL=air-monitor-tunnel). Runs
# `cloudflared tunnel run <name>` as a child process and cleans it up on shutdown.
# --------------------------------------------------------------------------- #
import shutil
import subprocess


def _start_cloudflared():
    print("not doing tunnel stuff for now. sorry!")
"""     name = os.environ.get("CLOUDFLARED_TUNNEL")
    if not name:
        return None
    exe = shutil.which("cloudflared")
    if not exe:
        print("[tunnel] CLOUDFLARED_TUNNEL set but `cloudflared` is not installed — skipping")
        return None
    try:
        proc = subprocess.Popen([exe, "tunnel", "run", name])
        print(f"[tunnel] cloudflared tunnel run {name} (pid {proc.pid})")
        return proc
    except Exception as e:
        print(f"[tunnel] failed to start cloudflared: {e}")
        return None """


# --------------------------------------------------------------------------- #
# App
# --------------------------------------------------------------------------- #
async def _broadcast_weather(observations) -> None:
    """Push freshly-stored weather obs to any open dashboards."""
    for o in observations:
        await hub.broadcast({"type": "weather", "data": {
            "valid_ts": o.valid_ts, "source": o.source, "station_id": o.station_id,
            "kind": o.kind, "distance_km": o.distance_km, **o.values,
        }})


# Aircraft: the current in-range snapshot (live only), refreshed by the poll loop.
_aircraft_snapshot: list = []
_sdr_status: dict = {"connected": None, "name": None}
_sdr_checked_at: float = 0.0


def _sdr_status_cached(ttl: float = 5.0) -> dict:
    """RTL-SDR presence, re-probed at most every `ttl` seconds (sysfs is cheap but
    the snapshot broadcast fires ~every second)."""
    global _sdr_status, _sdr_checked_at
    now = time.monotonic()
    if now - _sdr_checked_at >= ttl:
        _sdr_status = aircraft_usb.rtlsdr_status()
        _sdr_checked_at = now
    return _sdr_status


async def _on_aircraft_snapshot(records) -> None:
    """Cache the latest snapshot for /api/aircraft and push it to dashboards."""
    global _aircraft_snapshot
    _aircraft_snapshot = records
    await hub.broadcast({"type": "aircraft", "ts": int(time.time()),
                         "sdr": _sdr_status_cached(),
                         "data": [a.as_dict() for a in records]})


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _conn, _ro_conn
    _conn = _init_db()
    _ro_conn = _open_ro_conn()   # after _init_db: the file and views must exist
    print(f"[air-monitor] DB: {DB_PATH}")
    aiozc, info = await _start_mdns()
    cloudflared = _start_cloudflared()   # opt-in remote access (CLOUDFLARED_TUNNEL=...)

    # Weather/air-quality importer: a background poll loop alongside mDNS.
    weather_config.load_dotenv(Path(__file__).parent / ".env")
    wsettings = weather_config.settings()
    weather_task = None
    if wsettings["enabled"]:
        providers = weather_config.build_providers()
        active = [p.name for p in providers if p.enabled()]
        print(f"[weather] {len(active)} provider(s) active: {', '.join(active)} "
              f"(poll {wsettings['poll_sec']}s)")
        weather_task = asyncio.create_task(weather_scheduler.run_loop(
            providers, _conn, _db_lock,
            poll_sec=wsettings["poll_sec"], on_stored=_broadcast_weather,
        ))

    # Aircraft (ADS-B via readsb): its own background poll loop.
    asettings = aircraft_config.settings()
    aircraft_task = None
    if asettings["enabled"]:
        aircraft_task = asyncio.create_task(aircraft_scheduler.run_loop(
            _conn, _db_lock, settings=asettings, on_snapshot=_on_aircraft_snapshot,
        ))
        print(f"[aircraft] polling {asettings['json_url'] or asettings['json_path']} "
              f"every {asettings['poll_sec']}s; home=({asettings['lat']},{asettings['lon']})")

    yield

    if weather_task is not None:
        weather_task.cancel()
        try:
            await weather_task
        except asyncio.CancelledError:
            pass
    if aircraft_task is not None:
        aircraft_task.cancel()
        try:
            await aircraft_task
        except asyncio.CancelledError:
            pass
    if aiozc is not None:
        await aiozc.async_unregister_service(info)
        await aiozc.async_close()
    if cloudflared is not None:
        cloudflared.terminate()
        try:
            cloudflared.wait(timeout=5)
        except Exception:
            cloudflared.kill()
    if _ro_conn is not None:
        _ro_conn.close()
        _ro_conn = None
    _conn.close()


app = FastAPI(title="Air Monitor", lifespan=lifespan)
app.add_middleware(GZipMiddleware, minimum_size=1000)

# --------------------------------------------------------------------------- #
# Cookie auth — gates the dashboard and all API routes; /ingest is exempted so
# the ESP32 can keep posting without credentials.
# --------------------------------------------------------------------------- #
_DASHBOARD_PASSWORD = station.dashboard_password()
_COOKIE_NAME = "air_monitor_auth"
# Stateless session token: HMAC of the password. Rotating the password
# invalidates all existing sessions automatically.
_SESSION_TOKEN = (
    hmac.new(_DASHBOARD_PASSWORD.encode(), b"air-monitor-session", hashlib.sha256).hexdigest()
    if _DASHBOARD_PASSWORD else None
)

_LOGIN_HTML = """\
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Air Monitor</title>
<style>
body{font-family:system-ui,sans-serif;display:flex;align-items:center;
     justify-content:center;min-height:100vh;margin:0;background:#111;color:#eee}
form{background:#1e1e1e;padding:2rem;border-radius:12px;display:flex;
     flex-direction:column;gap:1rem;min-width:260px}
h2{margin:0;font-size:1.1rem;color:#aaa}
input{padding:.75rem;border-radius:8px;border:1px solid #333;
      background:#111;color:#eee;font-size:1rem}
button{padding:.75rem;border-radius:8px;border:none;background:#2563eb;
       color:#fff;font-size:1rem;cursor:pointer}
button:hover{background:#1d4ed8}
.err{color:#f87171;font-size:.9rem;margin:0}
</style>
</head>
<body>
<form method="post">
  <h2>Air Monitor</h2>
  <input type="password" name="password" placeholder="Password"
         autofocus autocomplete="current-password">
  <button type="submit">Log in</button>
  <!--ERR-->
</form>
</body>
</html>"""


class _CookieAuth(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        if not _SESSION_TOKEN or request.url.path in ("/ingest", "/login", "/logout", "/ws"):
            return await call_next(request)
        if hmac.compare_digest(request.cookies.get(_COOKIE_NAME, ""), _SESSION_TOKEN):
            return await call_next(request)
        next_url = request.url.path
        return RedirectResponse(url=f"/login?next={next_url}", status_code=303)


app.add_middleware(_CookieAuth)


@app.get("/login", response_class=HTMLResponse, include_in_schema=False)
async def login_get():
    return HTMLResponse(_LOGIN_HTML.replace("<!--ERR-->", ""))


@app.post("/login", response_class=HTMLResponse, include_in_schema=False)
async def login_post(request: Request, password: str = Form(...)):
    next_url = request.query_params.get("next", "/")
    if not next_url.startswith("/") or next_url.startswith("//"):   # block open redirect incl. //evil.com
        next_url = "/"
    if _SESSION_TOKEN and hmac.compare_digest(password.encode(), _DASHBOARD_PASSWORD.encode()):
        resp = RedirectResponse(url=next_url, status_code=303)
        resp.set_cookie(_COOKIE_NAME, _SESSION_TOKEN,
                        httponly=True, samesite="lax", max_age=30 * 24 * 3600)
        return resp
    return HTMLResponse(
        _LOGIN_HTML.replace("<!--ERR-->", '<p class="err">Wrong password.</p>'),
        status_code=401,
    )


@app.get("/logout", include_in_schema=False)
async def logout():
    resp = RedirectResponse(url="/login", status_code=303)
    resp.delete_cookie(_COOKIE_NAME)
    return resp

STATIC_DIR = Path(__file__).parent / "static"


# --------------------------------------------------------------------------- #
# Gas-concentration helpers — approximate, qualitative (see docs/SENSORS.md).
# --------------------------------------------------------------------------- #
_CO_R0   = 41_900.0   # Ω clean-air baseline (measured: Vout=333 mV, RL=4.7 kΩ)
_HCHO_V0 = 216.0      # mV clean-air baseline voltage (used for Vs/V0 curve)

# (Vs/V0 ratio, ppm) pairs read from SMD1001 datasheet Fig 1 (log-log).
_HCHO_CURVE: list[tuple[float, float]] = [
    (1.22, 0.10), (1.45, 0.20), (1.78, 0.40), (1.87, 0.60),
    (2.12, 0.80), (2.20, 1.00), (2.65, 1.20),
]


def _co_ppm(co_rs: float | None) -> float | None:
    """CO estimate from sensor resistance — GM-702B log-log fit (SENSORS.md).
    Returns None when Rs is missing or implausibly low (saturated / short)."""
    if not co_rs or co_rs < 1000:
        return None
    ratio = co_rs / _CO_R0
    if ratio <= 0:
        return None
    ppm = 10 ** ((math.log10(ratio) - 0.4) / -0.45)
    return round(max(0.0, ppm), 1)


def _hcho_ppm(hcho_mv: float | None) -> float | None:
    """HCHO estimate from sensor voltage — SMD1001 log-log interpolation (SENSORS.md).
    Returns None when voltage is missing or below baseline (implausible)."""
    if not hcho_mv or hcho_mv <= 0:
        return None
    ratio = hcho_mv / _HCHO_V0   # Vs/V0
    if ratio < _HCHO_CURVE[0][0]:
        return 0.0                # below detection range
    if ratio >= _HCHO_CURVE[-1][0]:
        return _HCHO_CURVE[-1][1] # saturated at top of curve
    # Log-log linear interpolation between the two bracketing curve points.
    for (r1, p1), (r2, p2) in zip(_HCHO_CURVE, _HCHO_CURVE[1:]):
        if r1 <= ratio <= r2:
            t = (math.log10(ratio) - math.log10(r1)) / (math.log10(r2) - math.log10(r1))
            ppm = 10 ** (math.log10(p1) + t * (math.log10(p2) - math.log10(p1)))
            return round(ppm, 3)
    return None


def _flatten(row: sqlite3.Row) -> dict:
    """Merge the stored JSON payload with the db columns into one flat dict. The
    db `ts` (effective timestamp) overrides the payload's raw device ts so the
    dashboard plots every reading at its best-estimate real time."""
    out = json.loads(row["payload"])
    out["ts"] = row["ts"]                       # effective timestamp wins
    out["device"] = row["device"]
    out["ts_ok"] = bool(row["ts_ok"])
    keys = row.keys()
    if "ts_source" in keys:
        out["ts_source"] = row["ts_source"]
    if "device_ts" in keys:
        out["device_ts"] = row["device_ts"]
    # Derived gas concentrations (approximate — see docs/SENSORS.md).
    if (v := _co_ppm(out.get("co_rs"))) is not None:
        out["co_ppm"] = v
    if (v := _hcho_ppm(out.get("hcho_mv"))) is not None:
        out["hcho_ppm"] = v
    # Recalibrate battery %: firmware used BAT_EMPTY_V=3.0 V which showed 35% at brownout.
    # Recompute from bat_v using empirical empty threshold (3.45 V under WiFi load).
    bat_v = out.get("bat_v")
    if bat_v is not None:
        out["bat_pct"] = round(max(0.0, min(100.0, (bat_v - 3.45) / (4.2 - 3.45) * 100)), 1)
    return out


# ---- ingest ---------------------------------------------------------------- #
@app.post("/ingest")
async def ingest(request: Request):
    """Accept an envelope {dev,mode,buffered,boot,fw,records:[...]} or, for
    backward compatibility, a bare record dict or a list of records."""
    body = await request.json()

    dev = None
    if isinstance(body, dict) and "records" in body:
        records = body["records"]
        dev = body.get("dev")
        mode = body.get("mode", "normal")
        if dev:
            _update_device(dev, mode, body.get("buffered"), body.get("boot"), body.get("fw"))
    else:
        records = body if isinstance(body, list) else [body]

    received_at = int(time.time())
    # Per-boot anchor: the largest uptime in this batch ≈ captured at received_at,
    # so uptime-derived timestamps for that boot hang off it (preserves intra-boot order).
    anchor_up: dict[Any, int] = {}
    for rec in records:
        if isinstance(rec, dict) and rec.get("up_ms") is not None:
            b = rec.get("boot")
            anchor_up[b] = max(anchor_up.get(b, 0), int(rec["up_ms"]))

    stored = []
    corrected = 0
    async with _db_lock:
        # Delta-decode: carry forward omitted slow values, null = real gap (schema v3).
        _forward_fill(records, dev)
        boots: set[tuple] = set()   # (device, boot) pairs that arrived with a good clock
        for rec in records:
            if not isinstance(rec, dict):
                continue
            eff_ts, source = _effective_ts(rec, received_at, anchor_up.get(rec.get("boot")))
            if _dedupe_insert(rec, received_at, eff_ts, source):
                rec = {**rec, "ts": eff_ts, "ts_source": source}   # broadcast at effective time
                stored.append(rec)
            if source == "device" and rec.get("boot") is not None:
                boots.add((rec.get("dev") or rec.get("device"), rec.get("boot")))
        # A reliably-timed record lets us back-fill that boot's uptime-derived ones.
        for device, boot in boots:
            corrected += _backfill_times(device, boot)
        _conn.commit()
    if corrected:
        print(f"[time] back-filled {corrected} record(s) with a corrected timestamp")

    # push the freshly stored ones to any open dashboards
    for rec in stored:
        await hub.broadcast({"type": "reading", "data": rec})

    # Tell dashboards about the device's current state.
    dev = body.get("dev") if isinstance(body, dict) else None
    if dev and dev in DEVICE_STATE:
        await hub.broadcast({"type": "device", "data": DEVICE_STATE[dev]})

    # Hand back our wall-clock epoch so an ESP32 whose NTP never succeeded can
    # adopt an approximate time (good to ~network-latency, fine for timestamps).
    resp = {"ok": True, "received": len(records), "stored": len(stored),
            "server_time": int(time.time())}
    if dev and dev in PENDING_CMD:
        resp["command"] = {"set_mode": PENDING_CMD[dev]}
    if dev and DEVICE_CONFIG.get(dev):           # firmware applyServerConfig() reads this
        resp["config"] = DEVICE_CONFIG[dev]
    return resp


# ---- query ----------------------------------------------------------------- #
@app.get("/api/readings")
async def get_readings(
    since: int | None = None,
    until: int | None = None,
    limit: int = 5000,
    device: str | None = None,
):
    """Time-range query. `since`/`until` are unix epoch seconds."""
    sql = "SELECT ts, device, device_ts, ts_ok, ts_source, payload FROM readings WHERE 1=1"
    args: list[Any] = []
    if since is not None:
        sql += " AND ts >= ?"; args.append(int(since))
    if until is not None:
        sql += " AND ts <= ?"; args.append(int(until))
    if device:
        sql += " AND device = ?"; args.append(device)
    sql += " ORDER BY ts ASC LIMIT ?"; args.append(int(limit))

    flat = await _read_query(
        lambda c: [_flatten(r) for r in c.execute(sql, args).fetchall()])
    return JSONResponse(flat)


@app.get("/api/latest")
async def latest(device: str | None = None):
    sql = "SELECT ts, device, ts_ok, payload FROM readings"
    args: list[Any] = []
    if device:
        sql += " WHERE device = ?"; args.append(device)
    sql += " ORDER BY ts DESC LIMIT 1"
    async with _db_lock:
        row = _conn.execute(sql, args).fetchone()
    return JSONResponse(_flatten(row) if row else {})


@app.get("/api/metrics")
async def metrics():
    """Introspect recent rows for the set of numeric fields available to plot."""
    async with _db_lock:
        rows = _conn.execute(
            "SELECT payload FROM readings ORDER BY ts DESC LIMIT 200"
        ).fetchall()
    keys: set[str] = set()
    for r in rows:
        for k, v in json.loads(r["payload"]).items():
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                keys.add(k)
    keys -= {"ts", "up_ms", "boot"}   # bookkeeping fields, not real metrics
    return sorted(keys)


@app.get("/api/stats")
async def stats():
    async with _db_lock:
        row = _conn.execute(
            "SELECT COUNT(*) n, MIN(ts) lo, MAX(ts) hi FROM readings"
        ).fetchone()
        nev = _conn.execute("SELECT COUNT(*) n FROM events").fetchone()["n"]
    return {"readings": row["n"], "first_ts": row["lo"], "last_ts": row["hi"], "events": nev}


# ---- research / analysis views --------------------------------------------- #
@app.get("/api/research/daily")
async def research_daily(limit: int = 30):
    """Per-day rollup: flights overhead, loudness, >65/>70 dB counts (incl. night),
    avg PM2.5 / CO2. Backed by the `daily_summary` view."""
    rows = await _cached_read(f"daily:{limit}", lambda c: [
        dict(r) for r in c.execute("SELECT * FROM daily_summary LIMIT ?",
                                   (int(limit),)).fetchall()])
    return JSONResponse(rows)


@app.get("/api/research/loudness")
async def research_loudness(limit: int = 25):
    """Average + peak noise per aircraft type seen overhead — which types are loudest."""
    rows = await _cached_read(f"loudness:{limit}", lambda c: [
        dict(r) for r in c.execute("SELECT * FROM loudness_by_type LIMIT ?",
                                   (int(limit),)).fetchall()])
    return JSONResponse(rows)


@app.get("/api/research/noise_events")
async def research_noise_events(limit: int = 100, since: int | None = None):
    """Loud readings attributed to the closest aircraft overhead at that moment.
    Newest first. Backed by the `noise_with_aircraft` view."""
    sql = "SELECT * FROM noise_with_aircraft"
    args: list[Any] = []
    if since is not None:
        sql += " WHERE ts >= ?"; args.append(int(since))
    sql += " ORDER BY ts DESC LIMIT ?"; args.append(int(limit))
    rows = await _cached_read(f"noise_events:{limit}:{since}", lambda c: [
        dict(r) for r in c.execute(sql, args).fetchall()])
    return JSONResponse(rows)


# ---- Dutch / EU aircraft noise statistics -------------------------------- #

import math as _math
import datetime as _dt

def _lden_from_rows(rows: list, tz_offset_h: int = 1) -> dict:
    """Compute Dutch Lden, Lnight and N-counts from a list of payload dicts.

    Lden = 10 × log10( (1/24) × (12×L_day + 4×10^((L_eve+5)/10) + 8×10^((L_nig+10)/10)) )
    where L_x is the energy-average LAeq over that period (day 07-19, eve 19-23, night 23-07 local).

    N-counts: NAxx = number of records where noise_lamax (or noise_dba when lamax absent) >= xx dB(A).
    Returns a dict keyed by device, or "all" for the aggregate.
    """
    period_e = {d: {"day": [], "eve": [], "night": []} for d in ["all"]}

    for p in rows:
        dev = p.get("device") or p.get("dev") or "unknown"
        if dev not in period_e:
            period_e[dev] = {"day": [], "eve": [], "night": []}

        ts = p.get("ts")
        if not ts:
            continue
        # Local hour (CET = UTC+1)
        local_h = (_dt.datetime.utcfromtimestamp(ts) + _dt.timedelta(hours=tz_offset_h)).hour

        # Use LAmax if available (Dutch preferred), fall back to LAeq
        level = p.get("lamax") or p.get("noise_dba")
        if level is None:
            continue
        level = float(level)

        bucket = "day" if 7 <= local_h < 19 else "eve" if 19 <= local_h < 23 else "night"
        period_e[dev][bucket].append(level)
        period_e["all"][bucket].append(level)

    def energy_avg(levels):
        if not levels:
            return None
        return 10.0 * _math.log10(sum(10 ** (x / 10.0) for x in levels) / len(levels))

    def lden(d, e, n):
        parts = []
        if d is not None: parts.append((12, d, 0))
        if e is not None: parts.append((4,  e, 5))
        if n is not None: parts.append((8,  n, 10))
        if not parts:
            return None
        total = sum(w * 10 ** ((l + pen) / 10.0) for w, l, pen in parts)
        return 10.0 * _math.log10(total / 24.0)

    def ncounts(levels):
        return {
            "na55": sum(1 for x in levels if x >= 55),
            "na60": sum(1 for x in levels if x >= 60),
            "na65": sum(1 for x in levels if x >= 65),
            "na70": sum(1 for x in levels if x >= 70),
        }

    result = {}
    for dev, buckets in period_e.items():
        ld = energy_avg(buckets["day"])
        le = energy_avg(buckets["eve"])
        ln = energy_avg(buckets["night"])
        all_levels = buckets["day"] + buckets["eve"] + buckets["night"]
        result[dev] = {
            "lday":   round(ld, 1) if ld is not None else None,
            "levening": round(le, 1) if le is not None else None,
            "lnight": round(ln, 1) if ln is not None else None,
            "lden":   round(lden(ld, le, ln), 1) if lden(ld, le, ln) is not None else None,
            "n_events": len(all_levels),
            **ncounts(all_levels),
        }
    return result


@app.get("/api/noise_stats")
async def noise_stats(
    since: int | None = None,
    until: int | None = None,
    device: str | None = None,
):
    """Dutch/EU aircraft noise indicators: Lden, Lnight, NA55/60/65/70 event counts.

    `since`/`until` are unix timestamps.  If omitted, defaults to the last 24 h.
    The Lden formula uses local time CET (UTC+1) for day/evening/night split.
    """
    now = int(time.time())
    if since is None:
        since = now - 86400
    sql = "SELECT ts, device, payload FROM readings WHERE ts >= ?"
    args: list[Any] = [since]
    if until is not None:
        sql += " AND ts <= ?"; args.append(int(until))
    if device:
        sql += " AND device = ?"; args.append(device)
    sql += " ORDER BY ts ASC"

    def _fetch(c: sqlite3.Connection) -> list[dict]:
        payloads = []
        for r in c.execute(sql, args).fetchall():
            p = json.loads(r["payload"])
            p["device"] = r["device"]
            payloads.append(p)
        return payloads

    return JSONResponse(_lden_from_rows(await _read_query(_fetch)))


# ---- audio frequency bands ------------------------------------------------ #
@app.get("/api/audio_bands")
async def audio_bands(
    since: int | None = None,
    until: int | None = None,
    device: str | None = None,
    limit: int = 5000,
):
    """Time-series of mic frequency bands (9 A-weighted bands).

    Returns: [{"ts": unix_epoch, "device": "dev_id", "bands": [dB, dB, ...]}, ...]
    where bands are: 20-100, 100-200, 200-400, 400-800, 800-1600, 1600-3200, 3200-6400, 6400-12800, 12800-20000 Hz (nominal).
    """
    sql = "SELECT ts, device, payload FROM readings WHERE 1=1"
    args: list[Any] = []
    if since is not None:
        sql += " AND ts >= ?"; args.append(int(since))
    if until is not None:
        sql += " AND ts <= ?"; args.append(int(until))
    if device:
        sql += " AND device = ?"; args.append(device)
    sql += " ORDER BY ts ASC LIMIT ?"; args.append(int(limit))

    def _fetch(c: sqlite3.Connection) -> list[dict]:
        result = []
        for r in c.execute(sql, args).fetchall():
            bands = json.loads(r["payload"]).get("noise_bands", [])
            if bands:
                result.append({"ts": r["ts"], "device": r["device"], "bands": bands})
        return result

    return JSONResponse(await _read_query(_fetch))


# ---- device configuration ------------------------------------------------- #
# In-memory config per device. Persists only during this server session.
# To make it persistent, store in the database or a config file.
DEVICE_CONFIG: dict[str, dict] = {}  # dev -> {poll_interval_s, ...}

@app.get("/api/config/{device}")
async def get_device_config(device: str):
    """One device's pushed config (poll_interval_s, …). Delivered to the device in
    each /ingest reply; this is just for inspection from the dashboard/CLI."""
    return JSONResponse(DEVICE_CONFIG.get(device, {}))


@app.post("/api/config/{device}")
async def set_config(device: str, request: Request):
    """Set device configuration. Persists only until server restart.

    Example: {"poll_interval_s": 120}
    The device fetches this at startup and after each sync.
    """
    body = await request.json()
    if device not in DEVICE_CONFIG:
        DEVICE_CONFIG[device] = {}
    DEVICE_CONFIG[device].update(body)
    # In a production system, you'd persist this to disk or database here.
    return JSONResponse({
        "ok": True,
        "device": device,
        "config": DEVICE_CONFIG[device],
    })


# ---- device mode (testing) control --------------------------------------- #
@app.get("/api/devices")
async def get_devices():
    """Current per-device state, with any pending command attached."""
    out = []
    for dev, st in DEVICE_STATE.items():
        d = dict(st)
        d["pending"] = PENDING_CMD.get(dev)
        out.append(d)
    return JSONResponse(out)


@app.post("/api/device/{dev}/mode")
async def set_device_mode(dev: str, request: Request):
    """Queue a mode switch for a device. Applied next time it contacts /ingest
    (immediately while it's online in the boot window or in testing mode)."""
    body = await request.json()
    mode = body.get("mode")
    if mode not in ("testing", "normal", "power_saving"):
        return JSONResponse(
            {"error": "mode must be 'testing', 'normal', or 'power_saving'"},
            status_code=400,
        )
    PENDING_CMD[dev] = mode
    if dev in DEVICE_STATE:
        DEVICE_STATE[dev]["pending"] = mode
        await hub.broadcast({"type": "device", "data": {**DEVICE_STATE[dev], "pending": mode}})
    return {"ok": True, "dev": dev, "pending": mode}


# ---- weather / air quality ------------------------------------------------- #
@app.get("/api/weather")
async def get_weather(
    since: int | None = None,
    until: int | None = None,
    source: str | None = None,
    variable: str | None = None,
    limit: int = 20000,
):
    """Time-range query over the external weather/air-quality series. `valid_ts` is
    the join key against /api/readings `ts`."""
    rows = await _read_query(
        lambda c: weather_store.query(c, since=since, until=until,
                                      source=source, variable=variable, limit=limit))
    return JSONResponse(rows)


@app.get("/api/weather/sources")
async def weather_sources():
    """Active sources with last-fetch time and nearest-station distance."""
    async with _db_lock:
        rows = _conn.execute(
            "SELECT source, MAX(valid_ts) AS last_valid, MAX(fetched_at) AS last_fetch, "
            "COUNT(*) AS rows, MIN(distance_km) AS nearest_km "
            "FROM weather GROUP BY source ORDER BY source"
        ).fetchall()
    return JSONResponse([dict(r) for r in rows])


@app.get("/api/weather/metrics")
async def weather_metrics():
    """Numeric weather variables seen recently, each with a best-effort unit."""
    async with _db_lock:
        rows = _conn.execute(
            "SELECT payload FROM weather ORDER BY valid_ts DESC LIMIT 500"
        ).fetchall()
    keys: set[str] = set()
    for r in rows:
        for k, v in json.loads(r["payload"]).items():
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                keys.add(k)
    return [{"name": k, "unit": weather_config.UNITS.get(k, "")} for k in sorted(keys)]


# ---- aircraft (ADS-B) ------------------------------------------------------ #
@app.get("/api/config")
async def get_config():
    """Static-ish config the dashboard needs (home location for the map, SDR status,
    and any per-device pushed configs)."""
    s = aircraft_config.settings()
    return {"home": [s["lat"], s["lon"]], "aircraft_enabled": s["enabled"],
            "max_range_km": s["max_range_km"], "sdr": _sdr_status_cached(),
            "devices": DEVICE_CONFIG}


@app.get("/api/aircraft")
async def get_aircraft(range_km: float | None = None):
    """Current in-range aircraft snapshot, nearest first. `range_km` optional."""
    records = _aircraft_snapshot
    if range_km is not None:
        records = [a for a in records if a.distance_km <= range_km]
    return JSONResponse([a.as_dict() for a in records])


# ---- flight-path history --------------------------------------------------- #

@app.get("/api/aircraft/paths")
async def aircraft_paths(range_km: float = 1.0, hours: float = 48.0):
    """Historical sightings within range_km, for flight-path graphs. `points` carry
    per-sample geometry (incl. track, for direction chevrons); `meta` carries each
    aircraft's identity once per hex (for the click-to-identify popup)."""
    since = int(time.time()) - int(min(hours, 168) * 3600)
    rows = await _read_query(
        lambda c: aircraft_store.query(c, since=since, max_distance_km=range_km))
    pts = []
    meta: dict[str, dict] = {}
    for r in rows:
        pts.append({"hex": r["hex"], "ts": r["ts"], "lat": r["lat"], "lon": r["lon"],
                    "alt_baro": r["alt_baro"], "track": r["track"], "source": r["source"]})
        m = meta.setdefault(r["hex"], {})
        for k in ("flight", "type", "reg", "desc", "category", "operator", "year"):
            if not m.get(k) and r[k] is not None:
                m[k] = r[k]
    return JSONResponse({"points": pts, "meta": meta})


# ---- flight route lookup --------------------------------------------------- #
# Cache callsign → {from_city, to_city} for 24 h (routes don't change mid-day).
_route_cache: dict[str, tuple[float, dict]] = {}  # callsign → (fetched_at, result)
_ROUTE_TTL = 86400  # seconds
_ROUTE_CACHE_MAX = 512  # far more than a day of distinct callsigns overhead

@app.get("/api/route")
async def get_route(callsign: str):
    """Look up origin/destination city for a flight callsign via OpenSky Network."""
    cs = callsign.strip().upper()
    now = time.time()
    if cs in _route_cache:
        fetched_at, result = _route_cache[cs]
        if now - fetched_at < _ROUTE_TTL:
            return JSONResponse(result)
    try:
        import requests as _req
        resp = await asyncio.to_thread(
            lambda: _req.get(
                f"https://opensky-network.org/api/routes?callsign={cs}",
                timeout=4.0,
                headers={"User-Agent": "air-monitor/1.0"},
            )
        )
        if resp.status_code == 200:
            data = resp.json()
            route = data.get("route") or []
            result = {"route": route}
        else:
            result = {"route": []}
    except Exception:
        result = {"route": []}
    _route_cache[cs] = (now, result)
    if len(_route_cache) > _ROUTE_CACHE_MAX:
        for k in [k for k, (t, _) in _route_cache.items() if now - t >= _ROUTE_TTL]:
            del _route_cache[k]
        while len(_route_cache) > _ROUTE_CACHE_MAX:
            del _route_cache[min(_route_cache, key=lambda k: _route_cache[k][0])]
    return JSONResponse(result)


# ---- events (home mode) ---------------------------------------------------- #
@app.post("/api/event")
async def add_event(request: Request):
    body = await request.json()
    ts = int(body.get("ts") or time.time())
    kind = str(body.get("kind", "mark"))
    label = body.get("label")
    note = body.get("note")
    async with _db_lock:
        cur = _conn.execute(
            "INSERT INTO events (ts, kind, label, note) VALUES (?,?,?,?)",
            (ts, kind, label, note),
        )
        _conn.commit()
        eid = cur.lastrowid
    ev = {"id": eid, "ts": ts, "kind": kind, "label": label, "note": note}
    await hub.broadcast({"type": "event", "data": ev})
    return ev


@app.get("/api/events")
async def get_events(since: int | None = None, until: int | None = None):
    sql = "SELECT id, ts, kind, label, note FROM events WHERE 1=1"
    args: list[Any] = []
    if since is not None:
        sql += " AND ts >= ?"; args.append(int(since))
    if until is not None:
        sql += " AND ts <= ?"; args.append(int(until))
    sql += " ORDER BY ts ASC"
    async with _db_lock:
        rows = _conn.execute(sql, args).fetchall()
    return JSONResponse([dict(r) for r in rows])


# ---- live websocket -------------------------------------------------------- #
@app.websocket("/ws")
async def ws(ws: WebSocket):
    await hub.connect(ws)
    try:
        while True:
            await ws.receive_text()   # we don't expect input; keeps the socket open
    except Exception:
        pass
    finally:
        hub.disconnect(ws)


# ---- dashboard ------------------------------------------------------------- #
@app.get("/", response_class=HTMLResponse)
async def index():
    return HTMLResponse(
        (STATIC_DIR / "index.html").read_text(encoding="utf-8"),
        headers={"X-Robots-Tag": "noindex, nofollow"},
    )


@app.get("/robots.txt")
async def robots():
    return Response("User-agent: *\nDisallow: /\n", media_type="text/plain")


if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=PORT)
