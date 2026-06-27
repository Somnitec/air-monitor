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
import json
import os
import socket
import sqlite3
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from weather import config as weather_config
from weather import scheduler as weather_scheduler
from weather import store as weather_store

from aircraft import config as aircraft_config
from aircraft import scheduler as aircraft_scheduler
from aircraft import store as aircraft_store
from aircraft import usb as aircraft_usb

# --------------------------------------------------------------------------- #
# Storage
# --------------------------------------------------------------------------- #
DB_PATH = Path(os.environ.get("AIRMON_DB", Path(__file__).parent / "data" / "air-monitor.db"))
DB_PATH.parent.mkdir(parents=True, exist_ok=True)

_db_lock = asyncio.Lock()
_conn: sqlite3.Connection


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
        WHERE COALESCE(lamax, noise_dba) >= 65
        ORDER BY ts;

        -- Each loud-ish reading joined to the closest aircraft overhead within ±30 s.
        -- "Which plane made this noise?" — the basis for cause attribution.
        DROP VIEW IF EXISTS noise_with_aircraft;
        CREATE VIEW noise_with_aircraft AS
        SELECT r.ts,
               datetime(r.ts,'unixepoch','localtime') AS time,
               r.noise_dba, r.lamax,
               a.hex, a.flight, a.type, a.category, a.operator,
               a.alt_baro, a.gs, a.baro_rate,
               round(a.distance_km, 2) AS dist_km
        FROM readings r
        LEFT JOIN aircraft a ON a.id = (
            SELECT a2.id FROM aircraft a2
            WHERE ABS(a2.ts - r.ts) <= 30
            ORDER BY a2.distance_km ASC LIMIT 1)
        WHERE COALESCE(r.lamax, r.noise_dba) >= 60
        ORDER BY r.ts;

        -- Which aircraft TYPES are loudest (avg + peak), for types seen overhead (≤8 km).
        DROP VIEW IF EXISTS loudness_by_type;
        CREATE VIEW loudness_by_type AS
        SELECT a.type,
               COUNT(*) AS n_samples,
               round(AVG(r.noise_dba), 1) AS avg_dba,
               round(MAX(COALESCE(r.lamax, r.noise_dba)), 1) AS max_dba
        FROM aircraft a
        JOIN readings r ON ABS(r.ts - a.ts) <= 15
        WHERE a.distance_km <= 8 AND a.type IS NOT NULL AND r.noise_dba IS NOT NULL
        GROUP BY a.type
        HAVING n_samples >= 3
        ORDER BY avg_dba DESC;

        -- Per local day: flights overhead, loudness, event counts, air quality.
        -- "Clearly better air when they don't fly for a day?" lives here.
        DROP VIEW IF EXISTS daily_summary;
        CREATE VIEW daily_summary AS
        WITH days(day) AS (
            SELECT DISTINCT date(ts,'unixepoch','localtime') FROM readings
            UNION
            SELECT DISTINCT date(ts,'unixepoch','localtime') FROM aircraft)
        SELECT day,
            (SELECT COUNT(DISTINCT hex) FROM aircraft
               WHERE date(ts,'unixepoch','localtime')=day AND distance_km<=10) AS flights,
            (SELECT round(MAX(COALESCE(lamax,noise_dba)),1) FROM readings
               WHERE date(ts,'unixepoch','localtime')=day) AS max_db,
            (SELECT COUNT(*) FROM readings
               WHERE date(ts,'unixepoch','localtime')=day AND COALESCE(lamax,noise_dba)>=65) AS n_ge65,
            (SELECT COUNT(*) FROM readings
               WHERE date(ts,'unixepoch','localtime')=day AND COALESCE(lamax,noise_dba)>=70) AS n_ge70,
            (SELECT COUNT(*) FROM readings
               WHERE date(ts,'unixepoch','localtime')=day AND COALESCE(lamax,noise_dba)>=65
                 AND (CAST(strftime('%H',ts,'unixepoch','localtime') AS INTEGER)<7
                   OR CAST(strftime('%H',ts,'unixepoch','localtime') AS INTEGER)>=23)) AS n_ge65_night,
            (SELECT round(AVG(pm25),1) FROM readings WHERE date(ts,'unixepoch','localtime')=day) AS avg_pm25,
            (SELECT round(AVG(co2),0)  FROM readings WHERE date(ts,'unixepoch','localtime')=day) AS avg_co2
        FROM days ORDER BY day DESC;
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
        dead = []
        for ws in self.clients:
            try:
                await ws.send_json(msg)
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


def _local_ip() -> str:
    """Best-effort primary LAN IP (the address other devices reach us on)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))   # no packets sent; just picks the route's source IP
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


async def _start_mdns():
    """Register the _airmon._tcp service. Uses AsyncZeroconf because we're inside
    a running asyncio loop (the sync Zeroconf API deadlocks here)."""
    try:
        from zeroconf import ServiceInfo
        from zeroconf.asyncio import AsyncZeroconf
    except ImportError:
        print("[mdns] zeroconf not installed — ESP32 must use the static SYNC_HOST fallback")
        return None, None
    ip = _local_ip()
    # Use a fixed hostname (not socket.gethostname(), which the ESP32 can't predict)
    # so the device can resolve our A-record directly via MDNS.queryHost("airmon-server").
    # zeroconf registers airmon-server.local. -> ip for us.
    info = ServiceInfo(
        "_airmon._tcp.local.",
        "air-monitor._airmon._tcp.local.",
        addresses=[socket.inet_aton(ip)],
        port=PORT,
        properties={"path": "/ingest"},
        server="airmon-server.local.",
    )
    aiozc = AsyncZeroconf()
    await aiozc.async_register_service(info)
    print(f"[mdns] advertising _airmon._tcp at {ip}:{PORT}")
    return aiozc, info


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
    global _conn
    _conn = _init_db()
    print(f"[air-monitor] DB: {DB_PATH}")
    aiozc, info = await _start_mdns()

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
    _conn.close()


app = FastAPI(title="Air Monitor", lifespan=lifespan)

STATIC_DIR = Path(__file__).parent / "static"


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
    return out


# ---- ingest ---------------------------------------------------------------- #
@app.post("/ingest")
async def ingest(request: Request):
    """Accept an envelope {dev,mode,buffered,boot,fw,records:[...]} or, for
    backward compatibility, a bare record dict or a list of records."""
    body = await request.json()

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

    async with _db_lock:
        rows = _conn.execute(sql, args).fetchall()
    return JSONResponse([_flatten(r) for r in rows])


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

    async with _db_lock:
        rows = _conn.execute(sql, args).fetchall()

    payloads = []
    for r in rows:
        p = json.loads(r["payload"])
        p["device"] = r["device"]
        payloads.append(p)

    return JSONResponse(_lden_from_rows(payloads))


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

    async with _db_lock:
        rows = _conn.execute(sql, args).fetchall()

    result = []
    for r in rows:
        p = json.loads(r["payload"])
        bands = p.get("noise_bands", [])
        if bands:
            result.append({
                "ts": r["ts"],
                "device": r["device"],
                "bands": bands,
            })
    return JSONResponse(result)


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
    if mode not in ("testing", "normal"):
        return JSONResponse({"error": "mode must be 'testing' or 'normal'"}, status_code=400)
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
    async with _db_lock:
        rows = weather_store.query(_conn, since=since, until=until,
                                   source=source, variable=variable, limit=limit)
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
    """Historical sightings within range_km, for flight-path graphs."""
    since = int(time.time()) - int(min(hours, 168) * 3600)
    rows = aircraft_store.query(_conn, since=since, limit=200_000)
    pts = [
        {"hex": r["hex"], "flight": r["flight"], "ts": r["ts"],
         "lat": r["lat"], "lon": r["lon"], "alt_baro": r["alt_baro"]}
        for r in rows
        if r["distance_km"] is not None and r["distance_km"] <= range_km
        and r["lat"] is not None and r["lon"] is not None
    ]
    return JSONResponse({"points": pts})


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
    except WebSocketDisconnect:
        hub.disconnect(ws)


# ---- dashboard ------------------------------------------------------------- #
@app.get("/", response_class=HTMLResponse)
async def index():
    return (STATIC_DIR / "index.html").read_text(encoding="utf-8")


if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=PORT)
