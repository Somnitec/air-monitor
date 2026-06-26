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
    conn.commit()
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
    _conn.execute(
        "INSERT INTO readings (ts, device, ts_ok, received_at, payload) VALUES (?,?,?,?,?)",
        (ts, device, ts_ok, int(time.time()), json.dumps(rec, separators=(",", ":"))),
    )
    return True


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
    info = ServiceInfo(
        "_airmon._tcp.local.",
        "air-monitor._airmon._tcp.local.",
        addresses=[socket.inet_aton(ip)],
        port=PORT,
        properties={"path": "/ingest"},
        server=f"{socket.gethostname()}.local.",
    )
    aiozc = AsyncZeroconf()
    await aiozc.async_register_service(info)
    print(f"[mdns] advertising _airmon._tcp at {ip}:{PORT}")
    return aiozc, info


# --------------------------------------------------------------------------- #
# App
# --------------------------------------------------------------------------- #
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _conn
    _conn = _init_db()
    print(f"[air-monitor] DB: {DB_PATH}")
    aiozc, info = await _start_mdns()
    yield
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
    return out


# ---- ingest ---------------------------------------------------------------- #
@app.post("/ingest")
async def ingest(request: Request):
    """Accept a single record object or a JSON array of records from the ESP32."""
    body = await request.json()
    records = body if isinstance(body, list) else [body]

    stored = []
    async with _db_lock:
        for rec in records:
            if not isinstance(rec, dict):
                continue
            if _dedupe_insert(rec):
                stored.append(rec)
        _conn.commit()

    # push the freshly stored ones to any open dashboards
    for rec in stored:
        await hub.broadcast({"type": "reading", "data": rec})

    return {"ok": True, "received": len(records), "stored": len(stored)}


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
    keys -= {"ts", "up_ms"}
    return sorted(keys)


@app.get("/api/stats")
async def stats():
    async with _db_lock:
        row = _conn.execute(
            "SELECT COUNT(*) n, MIN(ts) lo, MAX(ts) hi FROM readings"
        ).fetchone()
        nev = _conn.execute("SELECT COUNT(*) n FROM events").fetchone()["n"]
    return {"readings": row["n"], "first_ts": row["lo"], "last_ts": row["hi"], "events": nev}


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
