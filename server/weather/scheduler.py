"""The weather poll loop: concurrent fan-out, resilience, and gap backfill.

`poll_providers` fetches every enabled provider concurrently; one failing source is
captured as an error and never blocks the rest. `backfill_window` decides whether a
provider has been offline long enough to warrant pulling history. `run_loop` wires
these to the store and runs for the process lifetime, started from the app lifespan.
"""
from __future__ import annotations

import asyncio
import sqlite3
import time

from .base import Provider, WeatherObs
from . import store


async def poll_providers(providers: list[Provider]):
    """Fetch all enabled providers concurrently. Returns (observations, errors)
    where errors is a list of (provider_name, exception). Never raises."""
    active = [p for p in providers if p.enabled()]
    results = await asyncio.gather(*(p.fetch(None, None) for p in active),
                                   return_exceptions=True)
    obs: list[WeatherObs] = []
    errors: list[tuple[str, BaseException]] = []
    for p, r in zip(active, results):
        if isinstance(r, BaseException):
            errors.append((p.name, r))
        else:
            obs.extend(r)
    return obs, errors


def backfill_window(last_ts: int | None, *, now: int, threshold_s: int, max_days: int):
    """Return (since, until) to backfill, or None if the gap is small enough.
    With no history, fills the last `max_days`. A gap larger than `threshold_s`
    backfills from the last point (capped at `max_days`)."""
    floor = now - max_days * 86400
    if last_ts is None:
        return (floor, now)
    if now - last_ts <= threshold_s:
        return None
    return (max(last_ts, floor), now)


async def run_loop(providers: list[Provider], conn: sqlite3.Connection, db_lock,
                   *, poll_sec: int, on_stored=None,
                   backfill_threshold_s: int = 7200, backfill_max_days: int = 7):
    """Poll forever every `poll_sec`. On startup and after gaps, backfill history
    from providers that support it. `db_lock` is the asyncio lock guarding the shared
    sqlite connection; `on_stored(obs_list)` is an optional callback for live push."""
    # One-time backfill sweep on startup for history-capable providers.
    now = int(time.time())
    for p in providers:
        if not (p.enabled() and getattr(p, "supports_backfill", False)):
            continue
        async with db_lock:
            last = store.last_valid_ts(conn, p.name)
        win = backfill_window(last, now=now, threshold_s=backfill_threshold_s,
                              max_days=backfill_max_days)
        if win is None:
            continue
        try:
            hist = await p.backfill(*win)
            async with db_lock:
                store.upsert(conn, hist)
            print(f"[weather] backfilled {len(hist)} {p.name} record(s)")
        except Exception as e:
            print(f"[weather] backfill {p.name} failed: {e}")

    while True:
        obs, errors = await poll_providers(providers)
        if obs:
            async with db_lock:
                store.upsert(conn, obs)
            if on_stored is not None:
                await on_stored(obs)
        for name, exc in errors:
            print(f"[weather] {name} fetch failed: {exc}")
        print(f"[weather] polled {len(obs)} obs from "
              f"{len([p for p in providers if p.enabled()])} provider(s)")
        await asyncio.sleep(poll_sec)
