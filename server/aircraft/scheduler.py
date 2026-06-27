"""The aircraft poll loop: read readsb, locate vs home, broadcast live, log throttled.

Mirrors `weather/scheduler`: a testable single tick (`poll_once`) plus a forever
`run_loop` started from the app lifespan. Resilient to a missing/broken feed — that
is treated as "no aircraft right now" (empty snapshot), never an error that kills
the loop. The pure work lives in `base`/`source`/`store`.
"""
from __future__ import annotations

import asyncio
import sqlite3
import time

from . import store
from .base import merge_sources, normalize, select_for_logging
from .public import fetch_public
from .source import read_source


async def poll_once(conn: sqlite3.Connection, db_lock, *, settings: dict,
                    last_logged: dict[str, float], on_snapshot=None,
                    public_records: list | None = None):
    """One tick: read the local readsb snapshot, locate aircraft relative to home,
    merge in the cached `public_records` (cross-correlation), push via `on_snapshot`,
    and log the locally-received aircraft due per the throttle. Returns the merged
    records (empty list if neither feed is available)."""
    s = settings
    raw = read_source(s["json_path"], s["json_url"])
    local = ([] if raw is None
             else normalize(raw, s["lat"], s["lon"], stale_sec=s["stale_sec"],
                            max_range_km=s["max_range_km"], source="local"))
    records = merge_sources(local, public_records) if public_records else local
    if on_snapshot is not None:
        await on_snapshot(records)
    now = int(time.time())
    # Log all received aircraft (local SDR + public reference feed) so the DB
    # and side view populate even before readsb is running as a service.
    due = select_for_logging(records, last_logged, now=now, interval_sec=s["log_sec"])
    if due:
        async with db_lock:
            store.insert(conn, due, now=now)
    return records


async def _refresh_public(settings: dict) -> list:
    """Fetch + normalize the public reference feed (blocking IO off the event loop)."""
    s = settings
    raw = await asyncio.to_thread(fetch_public, s["lat"], s["lon"],
                                  s["public_radius_km"], url_template=s["public_url"])
    if raw is None:
        return []
    # Bound public planes by the (smaller) public radius, not the local SDR's range.
    return normalize(raw, s["lat"], s["lon"], stale_sec=s["stale_sec"],
                     max_range_km=s["public_radius_km"], source="public")


async def run_loop(conn: sqlite3.Connection, db_lock, *, settings: dict, on_snapshot=None):
    """Poll the local readsb snapshot every `poll_sec`. When enabled, refresh the
    public reference feed on its own slower cadence and merge it each tick."""
    last_logged: dict[str, float] = {}
    public_records: list = []
    last_public = 0.0
    while True:
        try:
            if settings["public_enabled"]:
                now = time.monotonic()
                if now - last_public >= settings["public_poll_sec"]:
                    last_public = now
                    public_records = await _refresh_public(settings)
            await poll_once(conn, db_lock, settings=settings, last_logged=last_logged,
                            on_snapshot=on_snapshot,
                            public_records=public_records if settings["public_enabled"] else None)
        except asyncio.CancelledError:
            raise
        except Exception as e:                       # never let one bad tick kill the loop
            print(f"[aircraft] poll error: {e}")
        await asyncio.sleep(settings["poll_sec"])
