"""SQLite persistence for aircraft sightings.

Append-only time-series: each call logs one row per aircraft at the sample time
`ts`. The poll loop throttles *how often* a given hex is logged (see
`aircraft.base.select_for_logging`); the store itself just records what it's given.
Separate from `readings` because the shape and cadence differ.
"""
from __future__ import annotations

import sqlite3
import time
from typing import Any

from .base import Aircraft

_DDL = """
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
    rssi        REAL,
    source      TEXT DEFAULT 'local'   -- 'local' | 'public' | 'both'
);
CREATE INDEX IF NOT EXISTS idx_aircraft_ts  ON aircraft(ts);
CREATE INDEX IF NOT EXISTS idx_aircraft_hex ON aircraft(hex);
"""


def init_aircraft_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    # Migrate older DBs that predate the source column.
    cols = {r[1] for r in conn.execute("PRAGMA table_info(aircraft)")}
    if "source" not in cols:
        conn.execute("ALTER TABLE aircraft ADD COLUMN source TEXT DEFAULT 'local'")
    conn.commit()


def insert(conn: sqlite3.Connection, aircraft: list[Aircraft], *,
           now: int | None = None) -> int:
    """Log a batch of aircraft, all stamped with the same sample time `ts`.
    Returns the number of rows written."""
    now = int(time.time()) if now is None else int(now)
    rows = [
        (now, a.hex, a.flight, a.type, a.reg, a.lat, a.lon, a.alt_baro,
         a.gs, a.track, a.baro_rate, a.distance_km, a.bearing_deg, a.rssi,
         a.source)
        for a in aircraft
    ]
    conn.executemany(
        "INSERT INTO aircraft "
        "(ts, hex, flight, type, reg, lat, lon, alt_baro, gs, track, "
        " baro_rate, distance_km, bearing_deg, rssi, source) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        rows,
    )
    conn.commit()
    return len(rows)


def query(conn: sqlite3.Connection, *, since: int | None = None,
          until: int | None = None, hex: str | None = None,
          limit: int = 20000) -> list[dict[str, Any]]:
    """Time-range / per-hex query of logged sightings, oldest first."""
    sql = "SELECT * FROM aircraft WHERE 1=1"
    args: list[Any] = []
    if since is not None:
        sql += " AND ts >= ?"; args.append(int(since))
    if until is not None:
        sql += " AND ts <= ?"; args.append(int(until))
    if hex:
        sql += " AND hex = ?"; args.append(hex)
    sql += " ORDER BY ts ASC LIMIT ?"; args.append(int(limit))
    return [dict(r) for r in conn.execute(sql, args).fetchall()]
