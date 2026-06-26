"""Read the readsb `aircraft.json` snapshot — from a local file or an HTTP URL.

Thin IO layer: returns the parsed dict, or None on any failure (missing file,
bad JSON, network error). The poll loop treats None as "no feed right now" and
keeps the dashboard alive. The pure work (filtering, range) lives in `base`.
"""
from __future__ import annotations

import json
from pathlib import Path

import requests


def read_source(path: str | None = None, url: str | None = None, *,
                timeout: float = 2.0) -> dict | None:
    """Return the parsed readsb snapshot, or None if it can't be read."""
    try:
        if url:
            resp = requests.get(url, timeout=timeout)
            resp.raise_for_status()
            return resp.json()
        if path:
            return json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception:
        return None
    return None
