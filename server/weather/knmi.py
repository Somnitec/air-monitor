"""KNMI Data Platform — official De Bilt 06260 observations (free API key).

KNMI serves 10-minute station data via the OGC EDR API as CoverageJSON. A position
query returns a `domain.axes.t` time axis plus a `ranges` block with one value array
per parameter (KNMI short codes). We map the codes we care about to friendly names
and emit one obs per timestamp. Enabled only when KNMI_API_KEY is set; if absent the
provider stays dormant and Open-Meteo (which ingests KNMI's model) covers the gap.

NOTE: parsing is unit-tested against the documented CoverageJSON shape; the live EDR
collection/parameter names must be confirmed with a real key (smoke test) — KNMI's
exact code set varies by dataset.
"""
from __future__ import annotations

from .base import Provider, WeatherObs, haversine_km, iso_to_epoch, num
from . import http

# KNMI parameter short code -> our friendly variable name.
_CODE_MAP = {
    "ta": "temp", "t": "temp",
    "td": "dew_point",
    "rh": "relative_humidity", "u": "relative_humidity",
    "pp": "pressure", "pres": "pressure", "p": "pressure",
    "ff": "wind_speed", "fx": "wind_gust",
    "dd": "wind_direction",
    "rg": "precipitation",
    "vv": "visibility",
    "qg": "global_radiation",
    "ss": "sunshine",
    "n": "cloud_cover",
}

# De Bilt reference station.
DEFAULT_STATION = "06260"
DEFAULT_LAT, DEFAULT_LON = 52.10, 5.18
_EDR = "https://api.dataplatform.knmi.nl/edr/v1/collections/10-minute-in-situ-meteorological-observations/locations/{station}"


def parse_edr(raw: dict, *, station_id: str, lat: float, lon: float,
              our_lat: float, our_lon: float) -> list[WeatherObs]:
    times = (((raw.get("domain") or {}).get("axes") or {}).get("t") or {}).get("values") or []
    ranges = raw.get("ranges") or {}
    dist = haversine_km(our_lat, our_lon, lat, lon)
    out: list[WeatherObs] = []
    for i, t in enumerate(times):
        values: dict[str, float] = {}
        for code, rng in ranges.items():
            arr = rng.get("values") if isinstance(rng, dict) else None
            if not isinstance(arr, list) or i >= len(arr):
                continue
            v = num(arr[i])
            if v is None:
                continue
            values[_CODE_MAP.get(code.lower(), code.lower())] = v
        out.append(WeatherObs("knmi", station_id, "obs", iso_to_epoch(t),
                              lat, lon, dist, values))
    return out


class Knmi(Provider):
    name = "knmi"

    def __init__(self, api_key, our_lat, our_lon,
                 station_id=DEFAULT_STATION, station_lat=DEFAULT_LAT, station_lon=DEFAULT_LON):
        self.api_key = api_key
        self.our_lat, self.our_lon = our_lat, our_lon
        self.station_id = station_id
        self.station_lat, self.station_lon = station_lat, station_lon

    def enabled(self) -> bool:
        return bool(self.api_key)

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        raw = await http.get_json(
            _EDR.format(station=self.station_id),
            params={"f": "CoverageJSON"},
            headers={"Authorization": self.api_key},
        )
        return parse_edr(raw, station_id=self.station_id,
                         lat=self.station_lat, lon=self.station_lon,
                         our_lat=self.our_lat, our_lon=self.our_lon)
