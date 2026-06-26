"""Open-Meteo — the keyless backbone.

Two endpoints, same `current` shape: general weather and (on a different host) air
quality. The ERA5 `archive` endpoint returns hourly arrays and is our gap-backfill
source. Source names are kept distinct ('open_meteo' vs 'open_meteo_aq') so the two
streams never collide on the (source, station, valid_ts, kind) dedupe key.
"""
from __future__ import annotations

from .base import Provider, WeatherObs, iso_to_epoch, num
from . import http

_SKIP = {"time", "interval"}

WEATHER_CURRENT = (
    "temperature_2m,relative_humidity_2m,dew_point_2m,apparent_temperature,"
    "pressure_msl,surface_pressure,precipitation,rain,cloud_cover,wind_speed_10m,"
    "wind_gusts_10m,wind_direction_10m,shortwave_radiation,direct_radiation,"
    "diffuse_radiation,visibility,cape"
)
AQ_CURRENT = (
    "pm10,pm2_5,carbon_monoxide,nitrogen_dioxide,sulphur_dioxide,ozone,"
    "aerosol_optical_depth,dust,uv_index,european_aqi"
)
BACKFILL_HOURLY = "temperature_2m,relative_humidity_2m,pressure_msl,wind_speed_10m,wind_direction_10m,precipitation"


def parse_current(raw: dict, *, source: str) -> list[WeatherObs]:
    """One WeatherObs from the `current` block."""
    cur = raw.get("current") or {}
    ts = cur.get("time")
    if ts is None:
        return []
    values = {k: num(v) for k, v in cur.items()
              if k not in _SKIP and num(v) is not None}
    return [WeatherObs(source, "", "obs", iso_to_epoch(ts),
                       raw.get("latitude"), raw.get("longitude"), 0.0, values)]


def parse_hourly(raw: dict, *, source: str) -> list[WeatherObs]:
    """ERA5 archive `hourly` arrays -> one reanalysis obs per hour (for backfill)."""
    hourly = raw.get("hourly") or {}
    times = hourly.get("time") or []
    out: list[WeatherObs] = []
    for i, t in enumerate(times):
        values = {}
        for k, arr in hourly.items():
            if k == "time" or not isinstance(arr, list) or i >= len(arr):
                continue
            v = num(arr[i])
            if v is not None:
                values[k] = v
        out.append(WeatherObs(source, "", "reanalysis", iso_to_epoch(t),
                              raw.get("latitude"), raw.get("longitude"), 0.0, values))
    return out


class OpenMeteoWeather(Provider):
    name = "open_meteo"
    supports_backfill = True
    _HOST = "https://api.open-meteo.com/v1/forecast"
    _ARCHIVE = "https://archive-api.open-meteo.com/v1/archive"

    def __init__(self, lat: float, lon: float):
        self.lat, self.lon = lat, lon

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        raw = await http.get_json(self._HOST, params={
            "latitude": self.lat, "longitude": self.lon,
            "current": WEATHER_CURRENT, "wind_speed_unit": "ms",
        })
        return parse_current(raw, source=self.name)

    async def backfill(self, since: int, until: int) -> list[WeatherObs]:
        import time as _t
        sd = _t.strftime("%Y-%m-%d", _t.gmtime(since))
        ed = _t.strftime("%Y-%m-%d", _t.gmtime(until))
        raw = await http.get_json(self._ARCHIVE, params={
            "latitude": self.lat, "longitude": self.lon,
            "start_date": sd, "end_date": ed,
            "hourly": BACKFILL_HOURLY, "wind_speed_unit": "ms",
        }, timeout=40)
        return parse_hourly(raw, source=self.name)


class OpenMeteoAirQuality(Provider):
    name = "open_meteo_aq"
    _HOST = "https://air-quality-api.open-meteo.com/v1/air-quality"

    def __init__(self, lat: float, lon: float):
        self.lat, self.lon = lat, lon

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        raw = await http.get_json(self._HOST, params={
            "latitude": self.lat, "longitude": self.lon, "current": AQ_CURRENT,
        })
        return parse_current(raw, source=self.name)
