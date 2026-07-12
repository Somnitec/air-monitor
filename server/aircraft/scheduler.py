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
from .public import fetch_public
from .source import read_source


async def poll_once(conn: sqlite3.Connection, db_lock, *, settings: dict,
                    last_logged: dict[str, float], on_snapshot=None,
                    public_records: list | None = None):
    """One tick. Our own RTL-SDR (readsb) is the source of truth: whenever its feed
    is readable we show *only* its aircraft, tagged 'sdr'. The public reference feed
    is a fallback used solely when the dongle/readsb is unavailable (file missing /
    URL unreachable) — so unplugging the dongle transparently switches to internet
    data, and plugging it back in switches straight back. Returns the snapshot."""
    s = settings
    raw = read_source(s["json_path"], s["json_url"])
    if raw is not None:
        # Dongle is feeding — trust it exclusively.
        records = normalize(raw, s["lat"], s["lon"], stale_sec=s["stale_sec"],
                            max_range_km=s["max_range_km"], source="sdr",
                            drop_on_ground=s.get("drop_on_ground", True))
    else:
        # No local feed — fall back to the public reference feed (already tagged 'public').
        records = list(public_records or [])
    if on_snapshot is not None:
        await on_snapshot(records)
    now = int(time.time())
    # Log whatever we're actually showing, so the side view + path history populate.
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
    # Bound public planes by the public radius; drop tarmac traffic same as the SDR.
    return normalize(raw, s["lat"], s["lon"], stale_sec=s["stale_sec"],
                     max_range_km=s["public_radius_km"], source="public",
                     drop_on_ground=s.get("drop_on_ground", True))


async def run_loop(conn: sqlite3.Connection, db_lock, *, settings: dict, on_snapshot=None):
    """Poll the local readsb snapshot every `poll_sec`. Keep a warm copy of the public
    reference feed (refreshed on its own slower cadence) so that if the dongle drops,
    `poll_once` can fall back to it instantly. While the dongle is feeding, the public
    copy is just kept warm and never shown.

    The public refresh runs as a *background task*, never awaited inline: it needs the
    internet, and with none a `requests` call can block for the connect timeout (or a
    DNS hang) — which, if awaited here, would freeze the local readsb poll for seconds
    every cycle. Since the readsb feed is a local file that works offline, the SDR map
    must keep updating at `poll_sec` regardless. The task just drops its result into a
    shared holder; the next `poll_once` reads whatever is current."""
    last_logged: dict[str, float] = {}
    holder: dict[str, list] = {"public": []}   # updated in place by the refresher task
    refresh_task: asyncio.Task | None = None
    last_public = 0.0

    async def _do_refresh() -> None:
        # Drop the warm public copy on failure so a fallback never shows stale
        # internet positions. Never propagates — a dead feed is "SDR only", not fatal.
        try:
            holder["public"] = await _refresh_public(settings)
        except Exception as e:
            holder["public"] = []
            print(f"[aircraft] public feed unavailable (SDR only): {e}")

    try:
        while True:
            try:
                if settings["public_enabled"]:
                    now = time.monotonic()
                    # Launch at most one refresh at a time, on its own slow cadence.
                    if (now - last_public >= settings["public_poll_sec"]
                            and (refresh_task is None or refresh_task.done())):
                        last_public = now
                        refresh_task = asyncio.create_task(_do_refresh())
                await poll_once(conn, db_lock, settings=settings, last_logged=last_logged,
                                on_snapshot=on_snapshot,
                                public_records=holder["public"] if settings["public_enabled"] else None)
            except asyncio.CancelledError:
                raise
            except Exception as e:                   # never let one bad tick kill the loop
                print(f"[aircraft] poll error: {e}")
            await asyncio.sleep(settings["poll_sec"])
    finally:
        if refresh_task is not None and not refresh_task.done():
            refresh_task.cancel()
