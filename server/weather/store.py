"""SQLite persistence for weather/air-quality observations.

Mirrors the `readings` blob pattern (variables live in a verbatim JSON `payload`)
so pandas/CSV export generalise, while keeping per-station provenance in real
columns. Dedupe is on (source, station_id, valid_ts, kind): re-polling the same
hour overwrites with the latest value instead of duplicating.
"""
from __future__ import annotations

import json
import sqlite3
import time
from typing import Any

from .base import WeatherObs

_DDL = """
CREATE TABLE IF NOT EXISTS weather (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    valid_ts    INTEGER NOT NULL,
    fetched_at  INTEGER NOT NULL,
    source      TEXT NOT NULL,
    station_id  TEXT NOT NULL DEFAULT '',
    kind        TEXT NOT NULL DEFAULT 'obs',
    lat         REAL,
    lon         REAL,
    distance_km REAL,
    payload     TEXT NOT NULL,
    UNIQUE(source, station_id, valid_ts, kind)
);
CREATE INDEX IF NOT EXISTS idx_weather_valid  ON weather(valid_ts);
CREATE INDEX IF NOT EXISTS idx_weather_source ON weather(source, valid_ts);
"""


def init_weather_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    conn.commit()


def upsert(conn: sqlite3.Connection, observations: list[WeatherObs], *, now: int | None = None) -> int:
    """Insert observations; on dedupe-key conflict, refresh payload + fetched_at.
    Returns the number of rows written (inserted or updated)."""
    now = int(time.time()) if now is None else int(now)
    n = 0
    for obs in observations:
        conn.execute(
            "INSERT INTO weather "
            "(valid_ts, fetched_at, source, station_id, kind, lat, lon, distance_km, payload) "
            "VALUES (?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(source, station_id, valid_ts, kind) DO UPDATE SET "
            "  payload=excluded.payload, fetched_at=excluded.fetched_at, "
            "  lat=excluded.lat, lon=excluded.lon, distance_km=excluded.distance_km",
            (int(obs.valid_ts), now, obs.source, obs.station_id, obs.kind,
             obs.lat, obs.lon, obs.distance_km,
             json.dumps(obs.values, separators=(",", ":"))),
        )
        n += 1
    conn.commit()
    return n


def _flatten(row: sqlite3.Row) -> dict[str, Any]:
    out = json.loads(row["payload"])
    out.update({
        "valid_ts": row["valid_ts"], "source": row["source"],
        "station_id": row["station_id"], "kind": row["kind"],
        "distance_km": row["distance_km"],
    })
    return out


def query(conn: sqlite3.Connection, *, since: int | None = None, until: int | None = None,
          source: str | None = None, variable: str | None = None,
          limit: int = 20000) -> list[dict[str, Any]]:
    """Time-range query, flattened. `variable` filters to rows whose payload has it."""
    sql = "SELECT valid_ts, source, station_id, kind, distance_km, payload FROM weather WHERE 1=1"
    args: list[Any] = []
    if since is not None:
        sql += " AND valid_ts >= ?"; args.append(int(since))
    if until is not None:
        sql += " AND valid_ts <= ?"; args.append(int(until))
    if source:
        sql += " AND source = ?"; args.append(source)
    if variable:
        sql += " AND json_extract(payload, '$.' || ?) IS NOT NULL"; args.append(variable)
    sql += " ORDER BY valid_ts ASC LIMIT ?"; args.append(int(limit))
    return [_flatten(r) for r in conn.execute(sql, args).fetchall()]


def last_valid_ts(conn: sqlite3.Connection, source: str) -> int | None:
    row = conn.execute(
        "SELECT MAX(valid_ts) FROM weather WHERE source = ?", (source,)
    ).fetchone()
    return row[0] if row and row[0] is not None else None
