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

# --------------------------------------------------------------------------- #
# Storage
# --------------------------------------------------------------------------- #
DB_PATH = Path(os.environ.get("AIRMON_DB", Path(__file__).parent / "data" / "air-monitor.db"))
DB_PATH.parent.mkdir(parents=True, exist_ok=True)

_db_lock = asyncio.Lock()
_conn: sqlite3.Connection


def _init_db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL;")     # crash-resilient, good for a USB stick
    conn.execute("PRAGMA synchronous=NORMAL;")
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS readings (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            ts          INTEGER NOT NULL,          -- sensor unix epoch (UTC)
            device      TEXT,
            ts_ok       INTEGER DEFAULT 1,         -- 0 = ESP32 clock was unsynced
            ts_status   TEXT DEFAULT 'ok',         -- 'ok' | 'provisional' | 'corrected'
            received_at INTEGER NOT NULL,          -- server unix epoch when stored
            payload     TEXT NOT NULL              -- full JSON record, verbatim
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
        """
    )
    # Migrate older DBs that predate the ts_status column.
    cols = {r["name"] for r in conn.execute("PRAGMA table_info(readings)")}
    if "ts_status" not in cols:
        conn.execute("ALTER TABLE readings ADD COLUMN ts_status TEXT DEFAULT 'ok'")
        conn.execute("UPDATE readings SET ts_status='provisional' WHERE ts_ok=0")
    conn.commit()
    weather_store.init_weather_table(conn)   # external weather/air-quality series
    return conn


def _dedupe_insert(rec: dict[str, Any]) -> bool:
    """Insert one reading. Returns True if newly stored, False if a duplicate
    (same device+ts) was skipped — makes re-syncs after a crash idempotent."""
    ts = int(rec.get("ts", 0))
    device = rec.get("dev") or rec.get("device")
    ts_ok = 1 if rec.get("ts_ok", True) else 0

    cur = _conn.execute(
        "SELECT 1 FROM readings WHERE ts=? AND device IS ? LIMIT 1", (ts, device)
    )
    if cur.fetchone() is not None:
        return False
    status = "ok" if ts_ok else "provisional"
    _conn.execute(
        "INSERT INTO readings (ts, device, ts_ok, ts_status, received_at, payload) "
        "VALUES (?,?,?,?,?,?)",
        (ts, device, ts_ok, status, int(time.time()),
         json.dumps(rec, separators=(",", ":"))),
    )
    return True


def _backfill_times(device: Any, boot: Any) -> int:
    """Retroactively fix a boot's early records. The ESP32 logs with provisional
    (uptime-based) timestamps until its clock is set; every record carries up_ms
    and a per-boot id. Once any record of this boot has a reliable clock (ts_ok=1),
    we know boot_epoch = ts - up_ms/1000, so each earlier record's true time is
    boot_epoch + up_ms/1000. Only the db columns change; payload stays verbatim.
    Returns the number of rows corrected."""
    if boot is None:
        return 0
    row = _conn.execute(
        "SELECT ts - json_extract(payload,'$.up_ms')/1000.0 "
        "FROM readings "
        "WHERE device IS ? AND json_extract(payload,'$.boot')=? AND ts_ok=1 "
        "ORDER BY ts LIMIT 1",
        (device, boot),
    ).fetchone()
    if row is None or row[0] is None:
        return 0
    boot_epoch = row[0]
    cur = _conn.execute(
        "UPDATE readings "
        "SET ts = CAST(? + json_extract(payload,'$.up_ms')/1000.0 AS INTEGER), "
        "    ts_ok=1, ts_status='corrected' "
        "WHERE device IS ? AND json_extract(payload,'$.boot')=? AND ts_ok=0",
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

    yield

    if weather_task is not None:
        weather_task.cancel()
        try:
            await weather_task
        except asyncio.CancelledError:
            pass
    if aiozc is not None:
        await aiozc.async_unregister_service(info)
        await aiozc.async_close()
    _conn.close()


app = FastAPI(title="Air Monitor", lifespan=lifespan)

STATIC_DIR = Path(__file__).parent / "static"


def _flatten(row: sqlite3.Row) -> dict:
    """Merge the stored JSON payload with the db columns into one flat dict."""
    out = json.loads(row["payload"])
    out["ts"] = row["ts"]
    out["device"] = row["device"]
    out["ts_ok"] = bool(row["ts_ok"])
    out["ts_status"] = row["ts_status"] if "ts_status" in row.keys() else "ok"
    return out


# ---- ingest ---------------------------------------------------------------- #
@app.post("/ingest")
async def ingest(request: Request):
    """Accept a single record object or a JSON array of records from the ESP32."""
    body = await request.json()
    records = body if isinstance(body, list) else [body]

    stored = []
    corrected = 0
    async with _db_lock:
        boots: set[tuple] = set()   # (device, boot) pairs that arrived with a good clock
        for rec in records:
            if not isinstance(rec, dict):
                continue
            if _dedupe_insert(rec):
                stored.append(rec)
            if rec.get("ts_ok", True) and rec.get("boot") is not None:
                boots.add((rec.get("dev") or rec.get("device"), rec.get("boot")))
        # A reliably-timed record lets us back-fill that boot's earlier provisional ones.
        for device, boot in boots:
            corrected += _backfill_times(device, boot)
        _conn.commit()
    if corrected:
        print(f"[time] back-filled {corrected} record(s) with a corrected timestamp")

    # push the freshly stored ones to any open dashboards
    for rec in stored:
        await hub.broadcast({"type": "reading", "data": rec})

    # Hand back our wall-clock epoch so an ESP32 whose NTP never succeeded can
    # adopt an approximate time (good to ~network-latency, fine for timestamps).
    return {"ok": True, "received": len(records), "stored": len(stored),
            "server_time": int(time.time())}


# ---- query ----------------------------------------------------------------- #
@app.get("/api/readings")
async def get_readings(
    since: int | None = None,
    until: int | None = None,
    limit: int = 5000,
    device: str | None = None,
):
    """Time-range query. `since`/`until` are unix epoch seconds."""
    sql = "SELECT ts, device, ts_ok, payload FROM readings WHERE 1=1"
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
