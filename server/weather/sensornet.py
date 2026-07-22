"""Sensornet Amstelveen aircraft-noise posts (municipal project `amstelveen_vliegtuiggeluid`).

Two posts bracket the station along the runway-27 approach path: CC (UT035,
Catharina van Clevepark) ~1.16 km due WEST at the same latitude — same flights,
a few seconds apart — and JA (UT005, Jeanne d'Arclaan) ~0.62 km south. Their
dataserver serves 5 s LAeq/LASmax series; we downsample to 1-minute rows
(energy-mean LAeq, max LAmax) for the dashboard overlay and cross-calibration
QA. Full-resolution event windows are fetched on demand by analysis tooling,
not stored here.

Series/page ids come from the project page's embedded JSON (data-page-id 9483,
map block `locations`). The same endpoint feeds the public project dashboard;
we poll it gently — one batched request per cycle — with an identifying UA.
"""
from __future__ import annotations

import math
import time

from . import http
from .base import Provider, WeatherObs, haversine_km

PAGE_ID = 9483
DATASERVER = "https://project.sensornet.nl/dataserver3/"
USER_AGENT = "air-monitor-research/0.1 (personal noise study, Amstelveen)"

# post key -> (station_id, lat, lon, {metric suffix -> serie id})
POSTS: dict[str, tuple[str, float, float, dict[str, int]]] = {
    "cc": ("UT035", 52.32012, 4.86243, {"laeq": 107383, "lamax": 107403}),
    "ja": ("UT005", 52.31453, 4.88026, {"laeq": 107413, "lamax": 107433}),
}

# The posts are hardcoded Amstelveen infrastructure: only poll when this install
# actually runs near them, so a fork elsewhere doesn't hit the municipal server.
ENABLE_WITHIN_KM = 15.0


def energy_mean_db(vals: list[float]) -> float:
    """Leq combination of equal-duration LAeq slices (NOT the arithmetic mean:
    [40,50] combines to ~47.4 dB, not 45)."""
    return 10.0 * math.log10(sum(10.0 ** (v / 10.0) for v in vals) / len(vals))


def parse_ranges(raw: dict) -> dict[int, list[tuple[int, float]]]:
    """dataserver3 reply {"ranges":[{"id":"107383","data":[[ms,val|null],..]},..]}
    -> {serie_id: [(epoch_s, val), ...]} with nulls dropped."""
    out: dict[int, list[tuple[int, float]]] = {}
    for r in raw.get("ranges", []):
        try:
            sid = int(r.get("id"))
        except (TypeError, ValueError):
            continue
        pts = [(int(ms // 1000), float(v)) for ms, v in r.get("data", []) if v is not None]
        out[sid] = pts
    return out


def _minutes(points: list[tuple[int, float]], how: str) -> dict[int, float]:
    """Bucket 5 s samples into minute rows. how='eq' -> energy mean, 'max' -> max."""
    buckets: dict[int, list[float]] = {}
    for ts, v in points:
        buckets.setdefault(ts - ts % 60, []).append(v)
    if how == "max":
        return {m: round(max(vs), 1) for m, vs in buckets.items()}
    return {m: round(energy_mean_db(vs), 1) for m, vs in buckets.items()}


def build_obs(ranges: dict[int, list[tuple[int, float]]],
              st_lat: float, st_lon: float) -> list[WeatherObs]:
    """Merge the per-serie minute buckets into one WeatherObs per post per minute,
    with payload keys namespaced per post (laeq_cc, lamax_cc, ...) — the dashboard
    flattens all weather rows together, so shared keys would interleave stations."""
    obs: list[WeatherObs] = []
    for key, (station_id, lat, lon, series) in POSTS.items():
        per_minute: dict[int, dict[str, float]] = {}
        for suffix, sid in series.items():
            how = "max" if suffix == "lamax" else "eq"
            for minute, val in _minutes(ranges.get(sid, []), how).items():
                per_minute.setdefault(minute, {})[f"{suffix}_{key}"] = val
        dist = haversine_km(st_lat, st_lon, lat, lon)
        for minute, values in sorted(per_minute.items()):
            obs.append(WeatherObs(
                source="sensornet", station_id=station_id, kind="noise",
                valid_ts=minute, lat=lat, lon=lon,
                distance_km=round(dist, 2), values=values,
            ))
    return obs


class Sensornet(Provider):
    name = "sensornet"

    def __init__(self, lat: float, lon: float):
        self.lat, self.lon = lat, lon

    def enabled(self) -> bool:
        _sid, cc_lat, cc_lon, _series = POSTS["cc"]
        return haversine_km(self.lat, self.lon, cc_lat, cc_lon) <= ENABLE_WITHIN_KM

    async def fetch(self, since=None, until=None) -> list[WeatherObs]:
        now = int(time.time())
        frm = int(since) if since else now - 25 * 60   # cover 2 missed 10-min cycles
        services = [f"{PAGE_ID}|{sid}"
                    for _key, (_id, _la, _lo, series) in POSTS.items()
                    for sid in series.values()]
        raw = await http.get_json(DATASERVER, params={
            "from": frm, "to": until or now, "service[]": services,
        }, headers={"User-Agent": USER_AGENT})
        return build_obs(parse_ranges(raw), self.lat, self.lon)
