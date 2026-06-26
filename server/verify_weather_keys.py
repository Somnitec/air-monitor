#!/usr/bin/env python3
"""Live smoke test for the keyed weather providers (KNMI / Netatmo / WU).

The keyless providers are already verified against the real APIs by the unit tests'
fixtures + the importer running in production. These three need real credentials, so
run this once after you've put your keys in `server/.env`:

    python verify_weather_keys.py

For each keyed provider it reports: configured?  ->  fetched N obs  ->  a sample row,
or the exact error. Nothing is written to the database; this only exercises fetch().
"""
from __future__ import annotations

import asyncio
import os

from weather import config as weather_config


def _sample(obs):
    o = obs[0]
    vals = {k: o.values[k] for k in list(o.values)[:6]}
    dist = f"{o.distance_km:.1f} km" if o.distance_km is not None else "n/a"
    return f"station={o.station_id or '(point)'} dist={dist} valid_ts={o.valid_ts} {vals}"


async def _run_one(provider) -> None:
    name = provider.name
    if not provider.enabled():
        print(f"  • {name:8s} — not configured (set its vars in .env); skipped")
        return
    try:
        obs = await provider.fetch(None, None)
    except Exception as e:
        print(f"  ✗ {name:8s} — FETCH FAILED: {type(e).__name__}: {e}")
        return
    if not obs:
        print(f"  ⚠ {name:8s} — connected but returned 0 obs "
              f"(check station id / radius / that the station is reporting)")
        return
    print(f"  ✓ {name:8s} — {len(obs)} obs")
    print(f"             {_sample(obs)}")


async def main() -> None:
    weather_config.load_dotenv(os.path.join(os.path.dirname(__file__), ".env"))
    providers = weather_config.build_providers()
    keyed = [p for p in providers if p.name in ("knmi", "netatmo", "wu")]

    print("Keyed weather provider smoke test")
    print("=" * 40)
    if not keyed:
        print("No keyed providers constructed. Did you set any of KNMI_API_KEY / "
              "NETATMO_* / WU_* in server/.env?")
        # Still show what *would* be checked, with their dormant status.
        from weather.knmi import Knmi
        from weather.netatmo import Netatmo
        from weather.wu import WeatherUnderground
        s = weather_config.settings()
        keyed = [
            Knmi(os.environ.get("KNMI_API_KEY", ""), s["lat"], s["lon"]),
            Netatmo(s["lat"], s["lon"], s["radius_km"], os.environ.get("NETATMO_CLIENT_ID", ""),
                    os.environ.get("NETATMO_CLIENT_SECRET", ""), os.environ.get("NETATMO_REFRESH_TOKEN", "")),
            WeatherUnderground(os.environ.get("WU_API_KEY", ""), os.environ.get("WU_STATION_ID", "")),
        ]
    for p in keyed:
        await _run_one(p)
    print("=" * 40)
    print("Done. ✓ = working, ⚠ = connected-but-empty, ✗ = error, • = not configured.")


if __name__ == "__main__":
    asyncio.run(main())
