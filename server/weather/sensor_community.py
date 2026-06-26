"""sensor.community (Luftdaten) — dense community sensors, keyless.

The area filter returns recent records from sensors within a radius. We keep only
*outdoor* sensors and flatten their `sensordatavalues` into one obs per record,
tagging each with the originating sensor id and distance.
"""
from __future__ import annotations

from .base import Provider, WeatherObs, haversine_km, iso_to_epoch, num
from . import http

_AREA = "https://data.sensor.community/airrohr/v1/filter/area={lat},{lon},{km}"


def parse(raw: list, *, our_lat: float, our_lon: float) -> list[WeatherObs]:
    out: list[WeatherObs] = []
    for rec in raw or []:
        loc = rec.get("location") or {}
        if str(loc.get("indoor", "0")) not in ("0", "0.0", "False", "false"):
            continue  # skip indoor sensors — we want outdoor reference
        lat, lon = num(loc.get("latitude")), num(loc.get("longitude"))
        ts_iso = rec.get("timestamp")
        if ts_iso is None:
            continue
        values: dict[str, float] = {}
        for sv in rec.get("sensordatavalues") or []:
            vt, v = sv.get("value_type"), num(sv.get("value"))
            if vt and v is not None:
                values[vt] = v
        if not values:
            continue
        dist = haversine_km(our_lat, our_lon, lat, lon) if lat is not None and lon is not None else None
        sensor_id = str((rec.get("sensor") or {}).get("id", ""))
        # sensor.community timestamps are "YYYY-MM-DD HH:MM:SS" UTC
        ts = iso_to_epoch(ts_iso.replace(" ", "T"))
        out.append(WeatherObs("sensor_community", sensor_id, "obs", ts, lat, lon, dist, values))
    return out


class SensorCommunity(Provider):
    name = "sensor_community"

    def __init__(self, lat: float, lon: float, radius_km: float):
        self.lat, self.lon, self.radius_km = lat, lon, radius_km

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        url = _AREA.format(lat=self.lat, lon=self.lon, km=self.radius_km)
        raw = await http.get_json(url, timeout=40)
        return parse(raw, our_lat=self.lat, our_lon=self.lon)
