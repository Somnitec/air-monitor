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

# Weather variables unpacked from the JSON payload into real, queryable columns
# (generated VIRTUAL columns — no value duplication, payload stays source of truth).
# Covers what the project correlates against: temp, humidity, wind, precip + pollution.
WEATHER_METRIC_COLS = [
    "temperature_2m", "relative_humidity_2m", "dew_point_2m", "apparent_temperature",
    "pressure_msl", "precipitation", "rain", "cloud_cover", "shortwave_radiation",
    "wind_speed_10m", "wind_gusts_10m", "wind_direction_10m",
    "pm10", "pm2_5", "european_aqi", "no2", "o3", "uv_index",
    # Sensornet Amstelveen noise posts (1-min downsample; keys namespaced per post
    # because the dashboard flattens all weather rows into one series per key)
    "laeq_cc", "lamax_cc", "laeq_ja", "lamax_ja",
]


def _metric_col_ddl() -> str:
    return ",\n    ".join(
        f"{c} REAL GENERATED ALWAYS AS (json_extract(payload,'$.{c}')) VIRTUAL"
        for c in WEATHER_METRIC_COLS
    )


_DDL = f"""
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
    {_metric_col_ddl()},
    UNIQUE(source, station_id, valid_ts, kind)
);
CREATE INDEX IF NOT EXISTS idx_weather_valid  ON weather(valid_ts);
CREATE INDEX IF NOT EXISTS idx_weather_source ON weather(source, valid_ts);
"""


def init_weather_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    # Migrate older DBs: add any missing generated columns (table_xinfo lists them).
    cols = {r[1] for r in conn.execute("PRAGMA table_xinfo(weather)")}
    for c in WEATHER_METRIC_COLS:
        if c not in cols:
            conn.execute(
                f"ALTER TABLE weather ADD COLUMN {c} REAL "
                f"GENERATED ALWAYS AS (json_extract(payload,'$.{c}')) VIRTUAL"
            )
    # Human-readable view: local-time + the unpacked variables + provenance.
    conn.executescript(
        "DROP VIEW IF EXISTS weather_h;"
        "CREATE VIEW weather_h AS SELECT id,"
        " datetime(valid_ts,'unixepoch','localtime') AS time,"
        " source, round(distance_km,1) AS dist_km, "
        + ", ".join(WEATHER_METRIC_COLS)
        + " FROM weather ORDER BY valid_ts;"
    )
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
    # Order DESC so LIMIT keeps the *most recent* rows, then reverse to hand back
    # ascending-by-valid_ts (the contract callers/plots rely on). Ordering ASC here
    # meant a busy window — e.g. sensor_community's ~16k rows/24h — hit the cap on
    # the oldest rows and silently dropped everything recent, so today's weather
    # never reached the dashboard.
    sql += " ORDER BY valid_ts DESC LIMIT ?"; args.append(int(limit))
    rows = [_flatten(r) for r in conn.execute(sql, args).fetchall()]
    rows.reverse()
    return rows


def last_valid_ts(conn: sqlite3.Connection, source: str) -> int | None:
    row = conn.execute(
        "SELECT MAX(valid_ts) FROM weather WHERE source = ?", (source,)
    ).fetchone()
    return row[0] if row and row[0] is not None else None
