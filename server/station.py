"""Single source of truth for the station's location.

The firmware's `secrets.h` defines `STATION_LAT` / `STATION_LON`; this module reads
them so the server doesn't need its coordinates configured separately. Both the
weather and aircraft features resolve "home" through `coords()` so they always agree.

Resolution order:
  1. Explicit AIRMON_LAT / AIRMON_LON environment variables
  2. firmware/src/secrets.h  (canonical gitignored firmware secrets)
  3. <repo-root>/secrets.h   (root-level gitignored convenience copy)
  4. Paleis Soestdijk placeholder — warns loudly so it's obvious
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent          # server/
_REPO = _HERE.parent                             # repo root

# Candidate paths tried in priority order (stop at the first that has coords).
_CANDIDATE_PATHS = [
    _REPO / "firmware" / "src" / "secrets.h",   # canonical firmware secrets
    _REPO / "secrets.h",                         # root-level copy / symlink
]
# Allow full override from env (e.g. set in server/.env or systemd unit).
if os.environ.get("AIRMON_FIRMWARE_SECRETS"):
    _CANDIDATE_PATHS.insert(0, Path(os.environ["AIRMON_FIRMWARE_SECRETS"]))

_PLACEHOLDER = (52.179722, 5.284722)


def firmware_coords(path: str | os.PathLike) -> tuple[float | None, float | None]:
    """Parse STATION_LAT / STATION_LON `#define`s from a firmware secrets header.
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
    # Explicit env vars beat everything.
    env_lat = _env_float("AIRMON_LAT", None)
    env_lon = _env_float("AIRMON_LON", None)
    if env_lat is not None and env_lon is not None:
        print(f"[station] coords from env: ({env_lat}, {env_lon})", file=sys.stderr)
        return env_lat, env_lon

    # Walk candidate paths; use the first that yields both values.
    for path in _CANDIDATE_PATHS:
        lat, lon = firmware_coords(path)
        if lat is not None and lon is not None:
            print(f"[station] coords from {path}: ({lat}, {lon})", file=sys.stderr)
            return lat, lon

    print(
        "[station] WARNING: no secrets.h with STATION_LAT/STATION_LON found — "
        "using placeholder coords (Paleis Soestdijk). "
        "Create firmware/src/secrets.h or <repo-root>/secrets.h with your real location.",
        file=sys.stderr,
    )
    return _PLACEHOLDER


def dashboard_password() -> str:
    """Read DASHBOARD_PASSWORD from env or secrets.h. Returns '' if not configured."""
    env = os.environ.get("AIRMON_PASSWORD")
    if env is not None:
        return env
    for path in _CANDIDATE_PATHS:
        if path.exists():
            m = re.search(r'^\s*#define\s+DASHBOARD_PASSWORD\s+"([^"]+)"', path.read_text(), re.MULTILINE)
            if m:
                print(f"[station] dashboard password from {path}", file=sys.stderr)
                return m.group(1)
    print(
        "[station] WARNING: no DASHBOARD_PASSWORD in secrets.h or AIRMON_PASSWORD env"
        " — dashboard is unprotected",
        file=sys.stderr,
    )
    return ""


def _env_float(name: str, default):
    try:
        v = os.environ.get(name)
        return float(v) if v is not None else default
    except (TypeError, ValueError):
        return default
