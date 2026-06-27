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
    seen        REAL,                  -- secs since last message at sample time
    operator    TEXT,                  -- ownOp (e.g. "KLM Royal Dutch Airlines")
    desc        TEXT,                  -- model description (e.g. "AIRBUS A-320")
    category    TEXT,                  -- ADS-B emitter category (A3, A7, ...)
    year        TEXT,                  -- year of manufacture
    source      TEXT DEFAULT 'local'   -- 'local' | 'public' | 'both'
);
CREATE INDEX IF NOT EXISTS idx_aircraft_ts  ON aircraft(ts);
CREATE INDEX IF NOT EXISTS idx_aircraft_hex ON aircraft(hex);
"""

# Columns added after the initial release; each gets a plain ALTER on older DBs.
_ADDED_COLS = {
    "source": "TEXT DEFAULT 'local'", "seen": "REAL", "operator": "TEXT",
    "desc": "TEXT", "category": "TEXT", "year": "TEXT",
}


def init_aircraft_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    cols = {r[1] for r in conn.execute("PRAGMA table_info(aircraft)")}
    for name, decl in _ADDED_COLS.items():
        if name not in cols:
            conn.execute(f"ALTER TABLE aircraft ADD COLUMN {name} {decl}")
    # Human-readable view for casual DB browsing.
    conn.executescript(
        "DROP VIEW IF EXISTS aircraft_h;"
        "CREATE VIEW aircraft_h AS SELECT id,"
        " datetime(ts,'unixepoch','localtime') AS time, hex, flight, type, desc,"
        " category, operator, reg, year, alt_baro, gs, track, baro_rate,"
        " round(distance_km,2) AS dist_km, round(bearing_deg) AS brg, source"
        " FROM aircraft ORDER BY ts;"
    )
    conn.commit()


def insert(conn: sqlite3.Connection, aircraft: list[Aircraft], *,
           now: int | None = None) -> int:
    """Log a batch of aircraft, all stamped with the same sample time `ts`.
    Returns the number of rows written."""
    now = int(time.time()) if now is None else int(now)
    rows = [
        (now, a.hex, a.flight, a.type, a.reg, a.lat, a.lon, a.alt_baro,
         a.gs, a.track, a.baro_rate, a.distance_km, a.bearing_deg, a.rssi,
         a.seen, a.operator, a.desc, a.category, a.year, a.source)
        for a in aircraft
    ]
    conn.executemany(
        "INSERT INTO aircraft "
        "(ts, hex, flight, type, reg, lat, lon, alt_baro, gs, track, "
        " baro_rate, distance_km, bearing_deg, rssi, seen, operator, desc, "
        " category, year, source) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
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
