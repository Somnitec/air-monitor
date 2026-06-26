"""Netatmo community PWS network (OAuth).

`getpublicdata` returns nearby personal weather stations in a lat/lon bbox. Each
station's `measures` are split across module MACs: a temp/humidity module (res +
type arrays), a pressure module, and special rain/wind blocks. We merge them into
one obs at the latest timestamp. Enabled only when client id/secret/refresh token
are configured; tokens are refreshed via the OAuth refresh-token grant.

NOTE: parsing is unit-tested against the documented shape; run a live smoke test
once real credentials exist.
"""
from __future__ import annotations

import math

from .base import Provider, WeatherObs, haversine_km, num
from . import http

_PUBLICDATA = "https://api.netatmo.com/api/getpublicdata"
_TOKEN = "https://api.netatmo.com/oauth2/token"


def parse_publicdata(raw: dict, *, our_lat: float, our_lon: float) -> list[WeatherObs]:
    out: list[WeatherObs] = []
    for st in raw.get("body") or []:
        place = st.get("place") or {}
        loc = place.get("location") or [None, None]
        lon, lat = (loc[0], loc[1]) if len(loc) == 2 else (None, None)
        values: dict[str, float] = {}
        latest_ts = 0
        for _mac, blk in (st.get("measures") or {}).items():
            if "res" in blk and "type" in blk:           # temp/humidity module
                for ts_s, vals in blk["res"].items():
                    latest_ts = max(latest_ts, int(ts_s))
                    for name, v in zip(blk["type"], vals):
                        nv = num(v)
                        if nv is not None:
                            values[name.lower()] = nv
            else:                                         # rain / wind special blocks
                for name, v in blk.items():
                    nv = num(v)
                    if nv is not None:
                        values[name.lower()] = nv
        if not values or latest_ts == 0:
            continue
        dist = haversine_km(our_lat, our_lon, lat, lon) if lat is not None and lon is not None else None
        out.append(WeatherObs("netatmo", st.get("_id", ""), "obs", latest_ts, lat, lon, dist, values))
    return out


class Netatmo(Provider):
    name = "netatmo"

    def __init__(self, lat, lon, radius_km, client_id, client_secret, refresh_token):
        self.lat, self.lon, self.radius_km = lat, lon, radius_km
        self.client_id, self.client_secret = client_id, client_secret
        self.refresh_token = refresh_token
        self._access_token: str | None = None

    def enabled(self) -> bool:
        return bool(self.client_id and self.client_secret and self.refresh_token)

    async def _refresh(self) -> str:
        tok = await http.post_json(_TOKEN, data={
            "grant_type": "refresh_token", "refresh_token": self.refresh_token,
            "client_id": self.client_id, "client_secret": self.client_secret,
        })
        self._access_token = tok.get("access_token")
        # Netatmo rotates the refresh token on each use.
        self.refresh_token = tok.get("refresh_token", self.refresh_token)
        return self._access_token

    def _bbox(self, km: float):
        dlat = km / 111.0
        dlon = km / (111.0 * max(0.1, abs(math.cos(math.radians(self.lat)))))
        return self.lat - dlat, self.lat + dlat, self.lon - dlon, self.lon + dlon

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        token = await self._refresh()
        lat_sw, lat_ne, lon_sw, lon_ne = self._bbox(self.radius_km)
        raw = await http.get_json(_PUBLICDATA, params={
            "lat_sw": lat_sw, "lon_sw": lon_sw, "lat_ne": lat_ne, "lon_ne": lon_ne,
        }, headers={"Authorization": f"Bearer {token}"})
        return parse_publicdata(raw, our_lat=self.lat, our_lon=self.lon)
