"""RIVM Luchtmeetnet — official Dutch outdoor air-quality reference (keyless).

The measurements feed is long-format (one row per station+formula+time). We group
those into one WeatherObs per (station, timestamp), keying values by a lowercased
formula (PM25 -> pm25). Station coordinates come from a separately-built index so we
can compute distance and keep only nearby stations.
"""
from __future__ import annotations

from .base import Provider, WeatherObs, haversine_km, iso_to_epoch, num
from . import http

_STATIONS = "https://api.luchtmeetnet.nl/open_api/stations"
_MEASUREMENTS = "https://api.luchtmeetnet.nl/open_api/measurements"


def parse_measurements(raw: dict, coords: dict[str, tuple[float, float]],
                       *, our_lat: float, our_lon: float) -> list[WeatherObs]:
    """Group long-format measurements into per-(station, time) observations."""
    groups: dict[tuple[str, int], WeatherObs] = {}
    for m in raw.get("data") or []:
        station = m.get("station_number")
        ts_iso = m.get("timestamp_measured")
        formula = m.get("formula")
        val = num(m.get("value"))
        if not (station and ts_iso and formula) or val is None:
            continue
        ts = iso_to_epoch(ts_iso)
        key = (station, ts)
        if key not in groups:
            latlon = coords.get(station)
            dist = haversine_km(our_lat, our_lon, *latlon) if latlon else None
            lat, lon = latlon if latlon else (None, None)
            groups[key] = WeatherObs("luchtmeetnet", station, "obs", ts, lat, lon, dist, {})
        groups[key].values[formula.lower()] = val
    return list(groups.values())


class Luchtmeetnet(Provider):
    name = "luchtmeetnet"

    def __init__(self, lat: float, lon: float, radius_km: float):
        self.lat, self.lon, self.radius_km = lat, lon, radius_km
        self._coords: dict[str, tuple[float, float]] = {}

    async def _station_index(self) -> dict[str, tuple[float, float]]:
        """Build {station_number: (lat, lon)} for stations within radius. Cached for
        the process lifetime — station locations don't move."""
        if self._coords:
            return self._coords
        page = 1
        numbers: list[str] = []
        while True:
            raw = await http.get_json(_STATIONS, params={"page": page})
            numbers += [s["number"] for s in raw.get("data", []) if "number" in s]
            pg = raw.get("pagination") or {}
            if page >= (pg.get("last_page") or page):
                break
            page += 1
        for nbr in numbers:
            try:
                d = await http.get_json(f"{_STATIONS}/{nbr}")
                geo = (d.get("data") or {}).get("geometry") or {}
                lon, lat = geo.get("coordinates", [None, None])
                if lat is None:
                    continue
                if haversine_km(self.lat, self.lon, lat, lon) <= self.radius_km:
                    self._coords[nbr] = (lat, lon)
            except Exception:
                continue
        return self._coords

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        coords = await self._station_index()
        out: list[WeatherObs] = []
        for station in coords:
            raw = await http.get_json(_MEASUREMENTS, params={
                "station_number": station, "order_by": "timestamp_measured",
                "order_direction": "desc", "page": 1,
            })
            out += parse_measurements(raw, coords, our_lat=self.lat, our_lon=self.lon)
        return out
