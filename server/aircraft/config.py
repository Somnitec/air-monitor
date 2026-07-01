"""Aircraft feature settings, read from the environment.

The app loads a local `.env` before reading these (same mechanism the weather
feature uses), so values can live in `server/.env`. Station location reuses the
shared `AIRMON_LAT`/`AIRMON_LON` so weather and aircraft agree on "home".
Defaults are the generic Paleis Soestdijk placeholder — never a real address.
"""
from __future__ import annotations

import os

import station
from . import public


def _f(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def settings() -> dict:
    lat, lon = station.coords()  # env > firmware secrets.h > placeholder
    return {
        "enabled": os.environ.get("AIRCRAFT_ENABLED", "1") not in ("0", "false", "False", "no"),
        "lat": lat,
        "lon": lon,
        "json_path": os.environ.get("AIRCRAFT_JSON_PATH", "/run/readsb/aircraft.json"),
        "json_url": os.environ.get("AIRCRAFT_JSON_URL") or None,
        "poll_sec": _f("AIRCRAFT_POLL_SEC", 1.0),
        "stale_sec": _f("AIRCRAFT_STALE_SEC", 60.0),
        # Study focus: only planes within 5 km of home — the ones that actually fly
        # overhead and could plausibly drive local noise/air-quality. The SDR still
        # *receives* out to the radio horizon; this bounds what we log, show, and
        # correlate (and keeps the DB from filling with hundreds of distant tracks).
        # Widen later via AIRCRAFT_MAX_RANGE_KM once we know what correlates.
        "max_range_km": _f("AIRCRAFT_MAX_RANGE_KM", 5.0),
        "log_sec": _f("AIRCRAFT_LOG_SEC", 15.0),
        # Public reference feed (cross-correlation). On by default so the map still
        # populates when local reception is weak; set =0 to disable the outbound call.
        "public_enabled": os.environ.get("AIRCRAFT_PUBLIC_ENABLED", "1") not in ("0", "false", "False", "no"),
        "public_url": os.environ.get("AIRCRAFT_PUBLIC_URL", public.DEFAULT_URL),
        "public_poll_sec": _f("AIRCRAFT_PUBLIC_POLL_SEC", 10.0),   # be polite to the community API
        # Match the SDR focus above: when the dongle is unavailable, only pull the
        # public feed within the same 5 km so the fallback shows the same overhead
        # planes and nothing farther. Bounds the merged snapshot payload too.
        "public_radius_km": _f("AIRCRAFT_PUBLIC_RADIUS_KM", 5.0),
    }
