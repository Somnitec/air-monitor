"""Resolve and persist flight routes (origin/destination) by callsign.

ADS-B tells us *which* plane is overhead (callsign, type, reg) but never where it's
going. The route is a property of the callsign — the same flight number flies the
same city pair day to day — so we look it up from the free adsbdb community DB,
keyed on callsign alone, and cache it in a `routes` table.

Because the lookup needs only the callsign (never a live position), routes fill in
*retroactively*: `backfill_once` sweeps callsigns already logged in `aircraft` that
we have no route for and resolves them whenever the internet is up — so a plane
recorded during an offline stretch still gets its route later, once we're back on.

Caveat: adsbdb returns the route the callsign flies *now*. For scheduled traffic
that's the right city pair for historical rows too; charter/GA/reassigned callsigns
are the exceptions. Good enough for an overflight study, not a flight log book.
"""
from __future__ import annotations

import asyncio
import sqlite3
import time
from typing import Any

import requests

ADSBDB_URL = "https://api.adsbdb.com/v0/callsign/{callsign}"

# adsbdb has no route for lots of GA/military/odd callsigns; cache those "not found"
# so we don't re-hit the network for them constantly, but retry weekly in case the DB
# gains the route (or the callsign gets reused for a scheduled flight).
NEG_CACHE_TTL = 7 * 86400

# The route fields we store/return; `callsign`, `found`, `fetched_at` are metadata.
_FIELDS = ("origin_icao", "origin_iata", "origin_city", "origin_name",
           "dest_icao", "dest_iata", "dest_city", "dest_name", "airline")

_DDL = """
CREATE TABLE IF NOT EXISTS routes (
    callsign     TEXT PRIMARY KEY,      -- ADS-B callsign, upper-cased
    found        INTEGER NOT NULL,      -- 1 = adsbdb had a route, 0 = negative cache
    origin_icao  TEXT, origin_iata TEXT, origin_city TEXT, origin_name TEXT,
    dest_icao    TEXT, dest_iata TEXT, dest_city TEXT, dest_name TEXT,
    airline      TEXT,
    fetched_at   INTEGER NOT NULL       -- epoch secs of the last adsbdb lookup
);
"""


def init_routes_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    # Human-readable browsing view (only rows we actually resolved).
    conn.executescript(
        "DROP VIEW IF EXISTS routes_h;"
        "CREATE VIEW routes_h AS SELECT callsign,"
        " origin_city || ' (' || origin_iata || ')' AS origin,"
        " dest_city   || ' (' || dest_iata   || ')' AS destination, airline,"
        " datetime(fetched_at,'unixepoch','localtime') AS fetched"
        " FROM routes WHERE found = 1;"
    )
    conn.commit()


def _parse(payload: Any) -> dict | None:
    """adsbdb JSON → normalized route dict, or None if it carried no usable route."""
    resp = payload.get("response") if isinstance(payload, dict) else None
    fr = resp.get("flightroute") if isinstance(resp, dict) else None
    if not isinstance(fr, dict):
        return None
    o = fr.get("origin") or {}
    d = fr.get("destination") or {}
    if not o and not d:
        return None
    return {
        "origin_icao": o.get("icao_code"), "origin_iata": o.get("iata_code"),
        "origin_city": o.get("municipality"), "origin_name": o.get("name"),
        "dest_icao": d.get("icao_code"), "dest_iata": d.get("iata_code"),
        "dest_city": d.get("municipality"), "dest_name": d.get("name"),
        "airline": (fr.get("airline") or {}).get("name"),
    }


def fetch(callsign: str, *, timeout: float = 4.0,
          url_template: str = ADSBDB_URL) -> dict | None:
    """Blocking adsbdb lookup. Returns the parsed route, or None when adsbdb knows the
    callsign has no route (HTTP 404 / empty). Raises on network/HTTP error so the
    caller can tell "offline" (retry later) apart from "genuinely unknown" (cache it)."""
    resp = requests.get(url_template.format(callsign=callsign), timeout=timeout,
                        headers={"User-Agent": "air-monitor/1.0"})
    if resp.status_code == 404:          # adsbdb's "unknown callsign" — a real answer
        return None
    resp.raise_for_status()
    return _parse(resp.json())


def get(conn: sqlite3.Connection, callsign: str) -> dict | None:
    """The cached row for a callsign (found flag + fields + fetched_at), or None."""
    row = conn.execute("SELECT * FROM routes WHERE callsign = ?",
                       (callsign,)).fetchone()
    return dict(row) if row else None


def upsert(conn: sqlite3.Connection, callsign: str, route: dict | None,
           now: int) -> None:
    """Store a lookup result. `route=None` writes a negative-cache row (found=0)."""
    vals = route or {}
    sql = ("INSERT INTO routes (callsign, found, " + ", ".join(_FIELDS) + ", fetched_at) "
           "VALUES (?, ?, " + ", ".join("?" * len(_FIELDS)) + ", ?) "
           "ON CONFLICT(callsign) DO UPDATE SET found = excluded.found, "
           + ", ".join(f"{c} = excluded.{c}" for c in _FIELDS)
           + ", fetched_at = excluded.fetched_at")
    conn.execute(sql, (callsign, 1 if route else 0,
                       *[vals.get(c) for c in _FIELDS], now))
    conn.commit()


def is_fresh(cached: dict, now: int) -> bool:
    """A cached row is fresh if it's a hit, or a miss we looked up recently."""
    return bool(cached["found"]) or (now - cached["fetched_at"] < NEG_CACHE_TTL)


def missing_callsigns(conn: sqlite3.Connection, *, limit: int, now: int) -> list[str]:
    """The backfill work-list: distinct callsigns logged in `aircraft` that have no
    route yet (or only a stale negative cache). Upper-cased to match how we store."""
    rows = conn.execute(
        "SELECT DISTINCT upper(a.flight) AS cs FROM aircraft a "
        "LEFT JOIN routes r ON r.callsign = upper(a.flight) "
        "WHERE a.flight IS NOT NULL AND a.flight <> '' "
        "  AND (r.callsign IS NULL OR (r.found = 0 AND r.fetched_at < ?)) "
        "LIMIT ?",
        (now - NEG_CACHE_TTL, limit),
    ).fetchall()
    return [r[0] for r in rows]


async def backfill_once(conn: sqlite3.Connection, db_lock, *, batch: int = 20,
                        pace_sec: float = 1.0, now: int | None = None) -> int:
    """Resolve up to `batch` not-yet-known callsigns from the log. Returns how many we
    resolved (hit or negatively cached). Politely paced — adsbdb is a free community
    service. The first lookup that *raises* (offline / rate-limited) ends the sweep;
    we retry next loop. Reads and writes go through `db_lock` on the write conn."""
    now = int(time.time()) if now is None else int(now)
    todo = await asyncio.to_thread(missing_callsigns, conn, limit=batch, now=now)
    resolved = 0
    for cs in todo:
        try:
            route = await asyncio.to_thread(fetch, cs)
        except Exception:
            break                        # offline — stop, the rest waits for next loop
        async with db_lock:
            await asyncio.to_thread(upsert, conn, cs, route, now)
        resolved += 1
        if pace_sec:
            await asyncio.sleep(pace_sec)
    return resolved


async def run_loop(conn: sqlite3.Connection, db_lock, *, poll_sec: float = 300.0,
                   batch: int = 20) -> None:
    """Periodically backfill missing routes. A dead internet just means zero resolved
    this cycle; it never raises out of here (mirrors the weather/aircraft loops)."""
    while True:
        try:
            n = await backfill_once(conn, db_lock, batch=batch)
            if n:
                print(f"[routes] backfilled {n} route(s)")
        except asyncio.CancelledError:
            raise
        except Exception as e:
            print(f"[routes] backfill error: {e}")
        await asyncio.sleep(poll_sec)
