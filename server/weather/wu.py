"""Weather Underground PWS observations (API key).

Pulls the current observation for a configured station id. The `metric` sub-block
holds the SI values; a few useful fields (humidity, winddir, solar, uv) sit at the
top level. Enabled only when an API key + station id are configured.

NOTE: parsing is unit-tested against the documented shape; run a live smoke test
once a real key + station exist.
"""
from __future__ import annotations

from .base import Provider, WeatherObs, iso_to_epoch, num
from . import http

_CURRENT = "https://api.weather.com/v2/pws/observations/current"
_TOPLEVEL = ("humidity", "winddir", "solarRadiation", "uv")


def parse_current(raw: dict) -> list[WeatherObs]:
    out: list[WeatherObs] = []
    for ob in raw.get("observations") or []:
        ts_iso = ob.get("obsTimeUtc")
        if ts_iso is None:
            continue
        values: dict[str, float] = {}
        for k, v in (ob.get("metric") or {}).items():
            nv = num(v)
            if nv is not None:
                values[k] = nv
        for k in _TOPLEVEL:
            nv = num(ob.get(k))
            if nv is not None:
                values[k] = nv
        out.append(WeatherObs("wu", ob.get("stationID", ""), "obs",
                              iso_to_epoch(ts_iso), num(ob.get("lat")), num(ob.get("lon")),
                              None, values))
    return out


class WeatherUnderground(Provider):
    name = "wu"

    def __init__(self, api_key, station_id):
        self.api_key, self.station_id = api_key, station_id

    def enabled(self) -> bool:
        return bool(self.api_key and self.station_id)

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        raw = await http.get_json(_CURRENT, params={
            "stationId": self.station_id, "format": "json",
            "units": "m", "apiKey": self.api_key,
        })
        return parse_current(raw)
