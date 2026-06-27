"""Single source of truth for the station's location.

The firmware's `secrets.h` defines `STATION_LAT` / `STATION_LON`; this module reads
them so the server doesn't need its coordinates configured separately. Both the
weather and aircraft features resolve "home" through `coords()` so they always agree.

Resolution order: explicit `AIRMON_LAT`/`AIRMON_LON` env wins, then the firmware
secrets header, then a generic Paleis Soestdijk placeholder (never a real address).
"""
from __future__ import annotations

import os
import re
from pathlib import Path

# Default to <repo>/firmware/src/secrets.h (this file lives at <repo>/server/station.py);
# override the path with AIRMON_FIRMWARE_SECRETS.
_FIRMWARE_SECRETS = os.environ.get(
    "AIRMON_FIRMWARE_SECRETS",
    str(Path(__file__).resolve().parents[1] / "firmware" / "src" / "secrets.h"),
)

_PLACEHOLDER = (52.179722, 5.284722)


def firmware_coords(path: str | os.PathLike = _FIRMWARE_SECRETS) -> tuple[float | None, float | None]:
    """Parse STATION_LAT / STATION_LON `#define`s from the firmware secrets header.
    Returns (None, None) if the file is missing or a value can't be parsed."""
    p = Path(path)
    if not p.exists():
        return None, None
    text = p.read_text()

    def _grab(name: str) -> float | None:
        m = re.search(rf"^\s*#define\s+{name}\s+([-+]?[0-9.]+)", text, re.MULTILINE)
        return float(m.group(1)) if m else None

    return _grab("STATION_LAT"), _grab("STATION_LON")


def coords() -> tuple[float, float]:
    """Resolve (lat, lon): env override > firmware secrets.h > placeholder."""
    fw_lat, fw_lon = firmware_coords()
    lat = _env_float("AIRMON_LAT", fw_lat if fw_lat is not None else _PLACEHOLDER[0])
    lon = _env_float("AIRMON_LON", fw_lon if fw_lon is not None else _PLACEHOLDER[1])
    return lat, lon


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default
