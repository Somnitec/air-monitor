"""Build the provider list from environment / a `.env` file.

Keyless providers are always on. Keyed providers (KNMI, Netatmo, WU) are added only
when their secrets are present, so the server runs fine with none of them configured.
Nothing here is committed — secrets live in the PC's environment or a local `.env`.
"""
from __future__ import annotations

import os
from pathlib import Path

import station
from .base import Provider
from .open_meteo import OpenMeteoWeather, OpenMeteoAirQuality
from .luchtmeetnet import Luchtmeetnet
from .sensor_community import SensorCommunity
from .knmi import Knmi
from .netatmo import Netatmo
from .wu import WeatherUnderground


def load_dotenv(path: str | os.PathLike = ".env") -> None:
    """Minimal KEY=VALUE loader (no dependency on python-dotenv). Existing env wins."""
    p = Path(path)
    if not p.exists():
        return
    for line in p.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))


def _f(name: str, default: float) -> float:
    return float(os.environ.get(name, default))


def settings() -> dict:
    lat, lon = station.coords()  # env > firmware secrets.h > placeholder
    return {
        "lat": lat,
        "lon": lon,
        "radius_km": _f("WEATHER_RADIUS_KM", 10.0),
        "poll_sec": int(os.environ.get("WEATHER_POLL_SEC", 600)),
        "enabled": os.environ.get("WEATHER_ENABLED", "1") not in ("0", "false", "False", "no"),
    }


def build_providers() -> list[Provider]:
    s = settings()
    lat, lon, radius = s["lat"], s["lon"], s["radius_km"]
    providers: list[Provider] = [
        OpenMeteoWeather(lat, lon),
        OpenMeteoAirQuality(lat, lon),
        Luchtmeetnet(lat, lon, radius),
        SensorCommunity(lat, lon, radius),
    ]
    if os.environ.get("KNMI_API_KEY"):
        providers.append(Knmi(os.environ["KNMI_API_KEY"], lat, lon))
    if all(os.environ.get(k) for k in ("NETATMO_CLIENT_ID", "NETATMO_CLIENT_SECRET", "NETATMO_REFRESH_TOKEN")):
        providers.append(Netatmo(lat, lon, radius,
                                 os.environ["NETATMO_CLIENT_ID"],
                                 os.environ["NETATMO_CLIENT_SECRET"],
                                 os.environ["NETATMO_REFRESH_TOKEN"]))
    if os.environ.get("WU_API_KEY") and os.environ.get("WU_STATION_ID"):
        providers.append(WeatherUnderground(os.environ["WU_API_KEY"], os.environ["WU_STATION_ID"]))
    return providers


# Variable -> unit, for the /api/weather/metrics hint. Best-effort; unknown vars omit a unit.
UNITS = {
    "temperature_2m": "°C", "temp": "°C", "apparent_temperature": "°C", "dew_point_2m": "°C",
    "dew_point": "°C", "relative_humidity_2m": "%", "relative_humidity": "%", "humidity": "%",
    "pressure_msl": "hPa", "surface_pressure": "hPa", "pressure": "hPa",
    "precipitation": "mm", "rain": "mm", "rain_live": "mm",
    "cloud_cover": "%", "cloud_cover_": "%",
    "wind_speed_10m": "m/s", "wind_speed": "m/s", "wind_strength": "km/h",
    "wind_gusts_10m": "m/s", "wind_gust": "m/s", "gust_strength": "km/h",
    "wind_direction_10m": "°", "wind_direction": "°", "wind_angle": "°", "winddir": "°",
    "shortwave_radiation": "W/m²", "direct_radiation": "W/m²", "diffuse_radiation": "W/m²",
    "global_radiation": "W/m²", "solarRadiation": "W/m²", "visibility": "m",
    "cape": "J/kg", "sunshine": "min",
    "pm10": "µg/m³", "pm2_5": "µg/m³", "pm25": "µg/m³",
    "carbon_monoxide": "µg/m³", "nitrogen_dioxide": "µg/m³", "no2": "µg/m³",
    "sulphur_dioxide": "µg/m³", "so2": "µg/m³", "ozone": "µg/m³", "o3": "µg/m³",
    "aerosol_optical_depth": "", "dust": "µg/m³", "uv_index": "", "uv": "",
    "european_aqi": "EAQI",
}
