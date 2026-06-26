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
from .base import normalize, select_for_logging
from .source import read_source


async def poll_once(conn: sqlite3.Connection, db_lock, *, settings: dict,
                    last_logged: dict[str, float], on_snapshot=None):
    """One tick: read the readsb snapshot, locate aircraft relative to home, push
    the snapshot via `on_snapshot`, and log each aircraft due per the throttle.
    Returns the current records (empty list if the feed is unavailable)."""
    s = settings
    raw = read_source(s["json_path"], s["json_url"])
    records = ([] if raw is None
               else normalize(raw, s["lat"], s["lon"],
                              stale_sec=s["stale_sec"], max_range_km=s["max_range_km"]))
    if on_snapshot is not None:
        await on_snapshot(records)
    now = int(time.time())
    due = select_for_logging(records, last_logged, now=now, interval_sec=s["log_sec"])
    if due:
        async with db_lock:
            store.insert(conn, due, now=now)
    return records


async def run_loop(conn: sqlite3.Connection, db_lock, *, settings: dict, on_snapshot=None):
    """Poll the readsb snapshot every `settings['poll_sec']` for the process life."""
    last_logged: dict[str, float] = {}
    while True:
        try:
            await poll_once(conn, db_lock, settings=settings,
                            last_logged=last_logged, on_snapshot=on_snapshot)
        except asyncio.CancelledError:
            raise
        except Exception as e:                       # never let one bad tick kill the loop
            print(f"[aircraft] poll error: {e}")
        await asyncio.sleep(settings["poll_sec"])
