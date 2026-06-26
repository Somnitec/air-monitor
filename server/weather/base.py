"""Foundation for weather/air-quality providers.

A `Provider` knows how to fetch *current observed* values from one external source
and turn them into `WeatherObs` rows. The HTTP-touching `fetch()` is kept thin; the
pure `parse()` does the work and is what the tests exercise (no network).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any


def iso_to_epoch(s: str) -> int:
    """Parse an ISO-8601 timestamp to unix epoch seconds. A naive timestamp (no
    offset, as Open-Meteo returns) is treated as UTC."""
    dt = datetime.fromisoformat(s)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return int(dt.timestamp())


def num(v: Any) -> float | None:
    """Coerce a value to float, or None if it isn't a finite number."""
    try:
        f = float(v)
    except (TypeError, ValueError):
        return None
    return f if math.isfinite(f) else None


def haversine_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Great-circle distance between two lat/lon points, in kilometres."""
    r = 6371.0  # mean Earth radius
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


@dataclass
class WeatherObs:
    """One observation: a bag of variable->value at a time/place, with provenance.

    `kind` is 'obs' for current observations or 'reanalysis' for gap-backfilled
    history. The store dedupes on (source, station_id, valid_ts, kind)."""
    source: str
    station_id: str
    kind: str
    valid_ts: int
    lat: float | None
    lon: float | None
    distance_km: float | None
    values: dict[str, Any] = field(default_factory=dict)

    def dedupe_key(self) -> tuple[str, str, int, str]:
        return (self.source, self.station_id, int(self.valid_ts), self.kind)


class Provider:
    """One external source. Subclasses set `name`, implement `enabled()` and the
    async `fetch(since, until)`. The HTTP-touching part should stay thin; parsing
    lives in pure module functions so it can be tested without a network."""

    name: str = "provider"

    def enabled(self) -> bool:
        return True

    async def fetch(self, since: int | None, until: int | None) -> list["WeatherObs"]:
        raise NotImplementedError

    # Most sources have no history; the scheduler calls this only for those that do.
    supports_backfill: bool = False

    async def backfill(self, since: int, until: int) -> list["WeatherObs"]:
        return []
