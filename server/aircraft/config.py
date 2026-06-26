"""Aircraft feature settings, read from the environment.

The app loads a local `.env` before reading these (same mechanism the weather
feature uses), so values can live in `server/.env`. Station location reuses the
shared `AIRMON_LAT`/`AIRMON_LON` so weather and aircraft agree on "home".
Defaults are the generic Paleis Soestdijk placeholder — never a real address.
"""
from __future__ import annotations

import os


def _f(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def settings() -> dict:
    return {
        "enabled": os.environ.get("AIRCRAFT_ENABLED", "1") not in ("0", "false", "False", "no"),
        "lat": _f("AIRMON_LAT", 52.179722),
        "lon": _f("AIRMON_LON", 5.284722),
        "json_path": os.environ.get("AIRCRAFT_JSON_PATH", "/run/readsb/aircraft.json"),
        "json_url": os.environ.get("AIRCRAFT_JSON_URL") or None,
        "poll_sec": _f("AIRCRAFT_POLL_SEC", 1.0),
        "stale_sec": _f("AIRCRAFT_STALE_SEC", 60.0),
        "max_range_km": _f("AIRCRAFT_MAX_RANGE_KM", 300.0),
        "log_sec": _f("AIRCRAFT_LOG_SEC", 15.0),
    }
