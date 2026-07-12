"""Schiphol runway-in-use ingester — authoritative labels for the overhead question.

We sit ~8 km due east of Schiphol under the Buitenveldertbaan (09/27). Whether planes
pass low overhead is entirely a function of *which runway Schiphol is using*, which is
driven mostly by wind. Rather than infer the active runway from our own ADS-B tracks
(fiddly geometry, parallel runways share a heading), we pull LVNL's actual runway usage
from a public keyless feed and store it as a labelled time series. That's ground truth
for "was it overhead", to correlate against wind and eventually forecast.

Source: https://www.dutchplanespotters.nl/api/runways/ams — republishes LVNL's
"actueel baangebruik" (updated ~every 5 min) as JSON: a list of {from, until,
landingRunways, departingRunways} intervals for the current day (history up to ~now;
it is NOT a forecast). Same feed the Home-Assistant "Schiphol Runway Monitor" uses.
"""
from __future__ import annotations

import asyncio
import datetime as dt
import json
import sqlite3
import time

from weather.http import get_json

DEFAULT_URL = "https://www.dutchplanespotters.nl/api/runways/ams"

# Runways whose traffic passes over THIS station. Landing 27 = final approach from the
# east, descending right over us; departing 09 = initial climb eastbound, over us. The
# other runways (Polderbaan/Zwanenburg/Aalsmeer 18-36, Kaagbaan 06/24) point elsewhere.
# Station-specific: revisit if the station moves. Landing 09 / departing 27 go the other
# way (west, over Schiphol itself), so they are NOT overhead here.
OVERHEAD_LANDING = {"27"}
OVERHEAD_DEPARTING = {"09"}


def is_overhead(landing: list[str], departing: list[str]) -> bool:
    """True if this runway config puts traffic low over the station."""
    return (any(r in OVERHEAD_LANDING for r in landing)
            or any(r in OVERHEAD_DEPARTING for r in departing))


def _epoch(iso: str) -> int:
    """ISO-8601 with offset (e.g. '2026-07-12T09:05:01+00:00') -> epoch seconds."""
    return int(dt.datetime.fromisoformat(iso).timestamp())


def parse_intervals(payload: dict) -> list[dict]:
    """Feed JSON -> normalized interval dicts, oldest first. Skips malformed rows
    rather than failing the whole poll (the feed is a hobbyist republisher)."""
    out: list[dict] = []
    for iv in payload.get("times", []):
        try:
            frm, until = _epoch(iv["from"]), _epoch(iv["until"])
        except (KeyError, TypeError, ValueError):
            continue
        landing = [str(r) for r in (iv.get("landingRunways") or [])]
        departing = [str(r) for r in (iv.get("departingRunways") or [])]
        out.append({
            "from_ts": frm, "until_ts": until,
            "landing": landing, "departing": departing,
            "overhead": is_overhead(landing, departing),
        })
    out.sort(key=lambda x: x["from_ts"])
    return out


_DDL = """
CREATE TABLE IF NOT EXISTS runway_use (
    from_ts    INTEGER PRIMARY KEY,   -- interval start (epoch); stable key for upsert
    until_ts   INTEGER NOT NULL,      -- interval end; the live interval's end grows each poll
    landing    TEXT NOT NULL,         -- JSON array, e.g. ["06","36R"]
    departing  TEXT NOT NULL,         -- JSON array, e.g. ["36L"]
    overhead   INTEGER NOT NULL,      -- 1 if this config is over the station
    fetched_at INTEGER NOT NULL       -- when we last saw/updated this interval
);
CREATE INDEX IF NOT EXISTS idx_runway_until ON runway_use(until_ts);
"""


def init_table(conn: sqlite3.Connection) -> None:
    conn.executescript(_DDL)
    conn.commit()


def upsert(conn: sqlite3.Connection, intervals: list[dict], *, now: int) -> int:
    """Insert new intervals / refresh the growing live one. Keyed on from_ts so each
    5-min re-fetch of the day updates the current interval's until rather than dup."""
    n = 0
    for iv in intervals:
        conn.execute(
            "INSERT INTO runway_use (from_ts, until_ts, landing, departing, overhead, fetched_at)"
            " VALUES (?,?,?,?,?,?)"
            " ON CONFLICT(from_ts) DO UPDATE SET until_ts=excluded.until_ts,"
            " landing=excluded.landing, departing=excluded.departing,"
            " overhead=excluded.overhead, fetched_at=excluded.fetched_at",
            (iv["from_ts"], iv["until_ts"], json.dumps(iv["landing"]),
             json.dumps(iv["departing"]), int(iv["overhead"]), now))
        n += 1
    conn.commit()
    return n


def _row(r: sqlite3.Row) -> dict:
    return {"from_ts": r["from_ts"], "until_ts": r["until_ts"],
            "landing": json.loads(r["landing"]), "departing": json.loads(r["departing"]),
            "overhead": bool(r["overhead"])}


def latest(conn: sqlite3.Connection) -> dict | None:
    """The most recent interval = current runway config (per the last poll)."""
    r = conn.execute("SELECT * FROM runway_use ORDER BY from_ts DESC LIMIT 1").fetchone()
    return _row(r) if r else None


def query(conn: sqlite3.Connection, *, since: int) -> list[dict]:
    """Intervals overlapping [since, now], oldest first — for the timeline + correlation."""
    rows = conn.execute(
        "SELECT * FROM runway_use WHERE until_ts >= ? ORDER BY from_ts ASC", (since,)
    ).fetchall()
    return [_row(r) for r in rows]


async def poll_once(conn: sqlite3.Connection, db_lock, *, url: str, on_update=None) -> dict | None:
    """One fetch+store tick. Returns the current (latest) config, or None on failure."""
    payload = await get_json(url, headers={"User-Agent": "air-monitor/1.0 (runway labels)"})
    intervals = parse_intervals(payload)
    if not intervals:
        return None
    now = int(time.time())
    async with db_lock:
        upsert(conn, intervals, now=now)
        cur = latest(conn)
    if on_update is not None and cur is not None:
        await on_update(cur)
    return cur


# ---- runway maintenance / closures (hand-maintained schedule) ---------------- #
# Schiphol publishes a yearly runway-maintenance schedule as a webpage, not an API
# (and it's Cloudflare-protected). It changes ~a few times a year, so a small editable
# JSON file is more robust than a brittle scraper. A Buitenveldertbaan (09/27) closure =
# guaranteed no overhead for those dates — the surest "quiet day" signal we have.

def _affects_overhead(closure: dict) -> bool:
    """Does this closure guarantee no overhead flights? True if it takes the
    Buitenveldertbaan (09/27) out of service."""
    rwy = str(closure.get("runway", ""))
    name = str(closure.get("name", "")).lower()
    return "09" in rwy or "27" in rwy or "buitenveld" in name


def _day_bounds(from_date: str, until_date: str) -> tuple[int, int]:
    """Inclusive local-day range -> [start_of_from 00:00, end_of_until 23:59:59] epoch.
    Uses the process TZ (the station runs on Europe/Amsterdam)."""
    d0 = dt.datetime.strptime(from_date, "%Y-%m-%d")
    d1 = dt.datetime.strptime(until_date, "%Y-%m-%d") + dt.timedelta(days=1)
    return int(d0.timestamp()), int(d1.timestamp()) - 1


def load_maintenance(path: str) -> list[dict]:
    """Read the closures file; return [] if missing/malformed (never fatal)."""
    try:
        with open(path) as f:
            data = json.load(f)
        return list(data.get("closures", []))
    except (OSError, ValueError):
        return []


def maintenance_status(closures: list[dict], *, now: int) -> dict:
    """Split closures into currently-active vs upcoming (future), each annotated with
    epoch bounds, days-until, and whether it removes overhead. Past closures dropped."""
    current, upcoming = [], []
    for c in closures:
        try:
            start, end = _day_bounds(c["from"], c["until"])
        except (KeyError, ValueError):
            continue
        if end < now:
            continue
        item = {**c, "from_ts": start, "until_ts": end,
                "affects_overhead": _affects_overhead(c)}
        if start <= now <= end:
            current.append(item)
        else:
            item["days_until"] = max(0, (start - now) // 86400)
            upcoming.append(item)
    upcoming.sort(key=lambda x: x["from_ts"])
    return {"current": current, "upcoming": upcoming}


async def run_loop(conn: sqlite3.Connection, db_lock, *, url: str = DEFAULT_URL,
                   poll_sec: float = 300.0, on_update=None):
    """Poll the runway feed forever. A dead feed (hobbyist site, no internet) is logged
    and retried next tick — never fatal, mirroring the aircraft/weather loops."""
    while True:
        try:
            await poll_once(conn, db_lock, url=url, on_update=on_update)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            print(f"[runway] poll error: {e}")
        await asyncio.sleep(poll_sec)
