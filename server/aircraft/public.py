"""Fetch a public ADS-B reference feed to cross-correlate against the local SDR.

Community APIs (airplanes.live, adsb.lol, adsb.fi) expose a point/radius query and
return the same tar1090/readsb aircraft shape — just under an `ac` key instead of
`aircraft`. We remap that to the readsb `{"aircraft": [...]}` form so `base.normalize`
handles it unchanged. Returns None on any failure (treated as "no reference feed").
"""
from __future__ import annotations

import requests

# {lat}/{lon}/{radius_nm}: aircraft within radius nautical miles (API caps at 250).
DEFAULT_URL = "https://api.airplanes.live/v2/point/{lat}/{lon}/{radius_nm}"

_KM_PER_NM = 1.852


def fetch_public(lat: float, lon: float, radius_km: float, *,
                 url_template: str = DEFAULT_URL, timeout: float = 4.0) -> dict | None:
    """Query the public feed around (lat, lon); return a readsb-shaped dict or None."""
    radius_nm = min(250, max(1, round(radius_km / _KM_PER_NM)))
    url = url_template.format(lat=lat, lon=lon, radius_nm=radius_nm)
    try:
        resp = requests.get(url, timeout=timeout, headers={"User-Agent": "air-monitor/1.0"})
        resp.raise_for_status()
        data = resp.json()
    except Exception:
        return None
    # Different feeds key the list as `ac` or `aircraft`; normalize to readsb's name.
    return {"aircraft": data.get("ac") or data.get("aircraft") or []}
