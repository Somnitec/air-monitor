#!/usr/bin/env python3
"""
Feed the server fake-but-plausible records so you can see the dashboard work
before the ESP32 firmware is flashed (handy while the battery pin is still TBD).

    python simulate.py                 # live: one record/sec to localhost:8000
    python simulate.py --backfill 24   # also POST 24 h of history first
    python simulate.py --host 192.168.1.50 --port 8000

It mimics the firmware's JSON shape exactly, including daily cycles so the
weekly/daily-pattern views have something to show.
"""
from __future__ import annotations

import argparse
import math
import random
import time

import requests

DEV = "sim-01"


def record(ts: int) -> dict:
    # daily phase 0..1
    day = (ts % 86400) / 86400.0
    diurnal = math.sin(2 * math.pi * (day - 0.25))      # peaks midday
    night = max(0.0, -diurnal)

    co2 = 480 + 350 * max(0, math.sin(2 * math.pi * (day - 0.1))) + random.gauss(0, 25)
    pm25 = max(0, 6 + 8 * night + random.gauss(0, 2))
    temp = 20 + 3 * diurnal + random.gauss(0, 0.2)
    rh = 50 - 8 * diurnal + random.gauss(0, 1)
    lux = max(0, 12000 * max(0, diurnal) + random.gauss(0, 50))
    noise = 38 + 12 * max(0, diurnal) + random.gauss(0, 3)
    rumble = abs(random.gauss(0, 0.05)) + (0.4 if random.random() < 0.03 else 0)

    return {
        "ts": ts, "ts_ok": True, "dev": DEV, "up_ms": ts * 1000,
        "pm1": round(pm25 * 0.7, 1), "pm25": round(pm25, 1),
        "pm4": round(pm25 * 1.1, 1), "pm10": round(pm25 * 1.3, 1),
        "co2": int(co2), "voc": round(100 + 30 * night + random.gauss(0, 5), 0),
        "nox": round(1 + 5 * night + random.gauss(0, 0.5), 1),
        "temp": round(temp, 2), "rh": round(rh, 1),
        "lux": round(lux, 1),
        "rumble": round(rumble, 4), "rumble_peak": round(rumble * 2.5, 4),
        "accel_mag": round(9.81 + random.gauss(0, 0.02), 3),
        "co_mv": int(330 + random.gauss(0, 8)), "co_rs": round(41900 + random.gauss(0, 800), 0),
        "hcho_mv": int(216 + random.gauss(0, 6)), "hcho_rs": round(142800 + random.gauss(0, 2000), 0),
        "soil_mv": int(2000 + random.gauss(0, 30)), "soil_pct": round(43 + random.gauss(0, 2), 0),
        "bat_raw_mv": int(820 + random.gauss(0, 5)), "bat_cal": False,
        "noise_dba": round(noise, 1), "noise_spl": round(noise + 4, 1),
        "noise_dbfs": round(noise - 94, 1), "noise_clip": False,
        "noise_bands": [round(noise - 6 + random.gauss(0, 2), 1) for _ in range(9)],
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--backfill", type=float, default=0, help="hours of history to POST first")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between live records")
    args = ap.parse_args()

    url = f"http://{args.host}:{args.port}/ingest"

    if args.backfill > 0:
        now = int(time.time())
        start = now - int(args.backfill * 3600)
        batch = [record(t) for t in range(start, now, 60)]   # 1/min like the firmware
        for i in range(0, len(batch), 200):
            requests.post(url, json=batch[i:i + 200], timeout=10)
        print(f"backfilled {len(batch)} records over {args.backfill} h")

    print(f"streaming live to {url} every {args.interval}s — Ctrl-C to stop")
    try:
        while True:
            r = record(int(time.time()))
            try:
                requests.post(url, json=r, timeout=5)
                print(f"  {r['ts']}  CO2={r['co2']} PM2.5={r['pm25']} {r['noise_dba']}dB(A)")
            except requests.RequestException as e:
                print(f"  POST failed: {e}")
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nstopped.")


if __name__ == "__main__":
    main()
