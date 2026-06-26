"""Tiny async HTTP-JSON helper.

The server runs in asyncio but only `requests` is available in the target
environment, so we run the blocking call in a worker thread. Keeps the providers'
`fetch()` one line and never blocks the event loop.
"""
from __future__ import annotations

import asyncio
from typing import Any

import requests


async def get_json(url: str, *, params: dict | None = None, headers: dict | None = None,
                   timeout: float = 20.0) -> Any:
    def _get():
        r = requests.get(url, params=params, headers=headers, timeout=timeout)
        r.raise_for_status()
        return r.json()
    return await asyncio.to_thread(_get)


async def post_json(url: str, *, data: dict | None = None, headers: dict | None = None,
                    timeout: float = 20.0) -> Any:
    def _post():
        r = requests.post(url, data=data, headers=headers, timeout=timeout)
        r.raise_for_status()
        return r.json()
    return await asyncio.to_thread(_post)
