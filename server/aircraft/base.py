"""Foundation for the aircraft (ADS-B) feature.

readsb decodes 1090 MHz and writes an `aircraft.json` snapshot. The pure
`normalize()` here turns that snapshot into `Aircraft` records enriched with
range + bearing from the station, dropping positionless, stale, or too-distant
entries. Kept network/IO-free so the tests exercise it directly.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any

from geo import bearing_deg, haversine_km

_FT_TO_KM = 0.0003048   # feet -> km


def overhead_geometry(distance_km: float, alt_baro: int | None) -> tuple[float | None, float | None]:
    """(slant_km, elev_deg) from horizontal range + barometric altitude. slant_km is the
    true 3-D distance to the aircraft (drives loudness, inverse-square); elev_deg is how
    high it sits in the sky (0=horizon, 90=straight up). Both None when altitude is
    unknown. NL is ~sea level so alt_baro (MSL) ≈ height above the station."""
    if alt_baro is None:
        return None, None
    alt_km = alt_baro * _FT_TO_KM
    slant = math.hypot(distance_km, alt_km)
    elev = math.degrees(math.atan2(alt_km, distance_km)) if distance_km > 0 else 90.0
    return round(slant, 3), round(elev, 1)


@dataclass
class Aircraft:
    """One aircraft in the current readsb snapshot, located relative to home."""
    hex: str
    flight: str | None
    type: str | None
    reg: str | None
    lat: float
    lon: float
    alt_baro: int | None     # ft; "ground" -> 0; missing -> None
    gs: float | None         # ground speed, kt
    track: float | None      # deg
    baro_rate: int | None    # ft/min (+climb / -descent)
    distance_km: float       # from home
    bearing_deg: float       # from home
    rssi: float | None
    seen: float              # seconds since last message
    operator: str | None = None   # ownOp from feed (e.g. "KLM Royal Dutch Airlines")
    desc: str | None = None       # aircraft model description (e.g. "AIRBUS A-320")
    category: str | None = None   # ADS-B emitter category (e.g. "A3", "A7")
    year: str | None = None       # year of manufacture
    source: str = "sdr"           # 'sdr' (our own RTL-SDR), 'public' (reference feed), or 'both'

    def as_dict(self) -> dict[str, Any]:
        slant_km, elev_deg = overhead_geometry(self.distance_km, self.alt_baro)
        return {
            "hex": self.hex, "flight": self.flight, "type": self.type,
            "reg": self.reg, "lat": self.lat, "lon": self.lon,
            "alt_baro": self.alt_baro, "gs": self.gs, "track": self.track,
            "baro_rate": self.baro_rate,
            "distance_km": round(self.distance_km, 2),
            "slant_km": slant_km, "elev_deg": elev_deg,
            "bearing_deg": round(self.bearing_deg, 1),
            "rssi": self.rssi, "seen": self.seen,
            "operator": self.operator, "desc": self.desc, "category": self.category,
            "year": self.year,
            "source": self.source,
        }


def _to_int(v: Any) -> int | None:
    try:
        return int(round(float(v)))
    except (TypeError, ValueError):
        return None


def _to_float(v: Any) -> float | None:
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _alt(v: Any) -> int | None:
    return 0 if v == "ground" else _to_int(v)


def normalize(raw: dict, home_lat: float, home_lon: float, *,
              stale_sec: float = 60.0, max_range_km: float = 300.0,
              source: str = "sdr", drop_on_ground: bool = False) -> list[Aircraft]:
    """Turn a parsed readsb `aircraft.json` dict into a distance-sorted list of
    `Aircraft`. Drops entries without a position, heard longer ago than
    `stale_sec`, or farther than `max_range_km` from home. `source` tags where the
    snapshot came from ('sdr' = our own RTL-SDR, or 'public' = reference feed).

    `drop_on_ground` discards aircraft reporting `alt_baro == "ground"` (alt 0). We
    sit ~7-9 km from Schiphol's aprons, so without this the log is dominated by
    taxiing/parked jets dwelling on the tarmac (once-per-15s each, for the whole time
    they sit there) — noise for an *overflight* study, and ~70-97% of all rows. Planes
    with no altitude at all (alt None) are kept: a real flyover with a flaky
    transponder shouldn't vanish."""
    out: list[Aircraft] = []
    for a in raw.get("aircraft", []):
        lat, lon = _to_float(a.get("lat")), _to_float(a.get("lon"))
        if lat is None or lon is None:
            continue
        seen = _to_float(a.get("seen")) or 0.0
        if seen > stale_sec:
            continue
        alt = _alt(a.get("alt_baro"))
        if drop_on_ground and alt == 0:
            continue
        dist = haversine_km(home_lat, home_lon, lat, lon)
        if dist > max_range_km:
            continue
        out.append(Aircraft(
            hex=a.get("hex", ""),
            flight=(a.get("flight") or "").strip() or None,
            type=a.get("t"),
            reg=a.get("r"),
            lat=lat, lon=lon,
            alt_baro=alt,
            gs=_to_float(a.get("gs")),
            track=_to_float(a.get("track")),
            baro_rate=_to_int(a.get("baro_rate", a.get("geom_rate"))),
            distance_km=dist,
            bearing_deg=bearing_deg(home_lat, home_lon, lat, lon),
            rssi=_to_float(a.get("rssi")),
            seen=seen,
            operator=(a.get("ownOp") or "").strip() or None,
            desc=(a.get("desc") or "").strip() or None,
            category=a.get("category"),
            year=str(a["year"]) if a.get("year") else None,
            source=source,
        ))
    out.sort(key=lambda x: x.distance_km)
    return out


def merge_sources(local: list[Aircraft], public: list[Aircraft]) -> list[Aircraft]:
    """Union our SDR and public snapshots by `hex` for cross-correlation. An aircraft
    seen by both is tagged 'both' and keeps the SDR record (real rssi/seen);
    SDR-only stays 'sdr', public-only becomes 'public'. Distance-sorted."""
    by_hex: dict[str, Aircraft] = {}
    for ac in local:
        ac.source = "sdr"
        by_hex[ac.hex] = ac
    for ac in public:
        if ac.hex in by_hex:
            by_hex[ac.hex].source = "both"
        else:
            ac.source = "public"
            by_hex[ac.hex] = ac
    out = list(by_hex.values())
    out.sort(key=lambda x: x.distance_km)
    return out


def select_for_logging(records: list[Aircraft], last_logged: dict[str, float], *,
                       now: float, interval_sec: float) -> list[Aircraft]:
    """Per-hex throttle: return the records due for logging (not logged within
    `interval_sec`) and stamp their time into `last_logged` (mutated in place)."""
    due: list[Aircraft] = []
    for ac in records:
        prev = last_logged.get(ac.hex)
        if prev is None or now - prev >= interval_sec:
            due.append(ac)
            last_logged[ac.hex] = now
    # The dict would otherwise keep one entry per hex ever seen (thousands a week
    # near Schiphol, for the process lifetime). An aircraft gone long enough gets
    # logged immediately on return anyway, so a stale entry carries no information.
    if len(last_logged) > 1000:
        cutoff = now - max(3600.0, 10 * interval_sec)
        for h in [h for h, t in last_logged.items() if t < cutoff]:
            del last_logged[h]
    return due
