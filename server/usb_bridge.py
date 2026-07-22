#!/usr/bin/env python3
"""USB serial bridge: sync the station over USB instead of WiFi.

Relays the firmware's "#SYNC# <json>" batch lines to the server's /ingest and
answers with the server's reply; heartbeats every ~2 s so the firmware knows a
host is listening (it then keeps its WiFi radio off entirely). Heartbeats carry
server_time, so the station's clock syncs without NTP.

    ./.venv/bin/python usb_bridge.py                                  # auto-detect port
    ./.venv/bin/python usb_bridge.py --port /dev/ttyUSB0 --server http://127.0.0.1:8000

Every non-#SYNC# line is the firmware's normal serial log, echoed to stdout —
this doubles as `pio device monitor` (and you can't run both at once: the port
is exclusive). Ctrl-C to stop; the station notices the silence within ~20 s and
falls back to POWER_SAVING + periodic WiFi.
"""
from __future__ import annotations

import argparse
import glob
import json
import sys
import time

import requests

try:
    import serial
except ImportError:
    sys.exit("pyserial missing — run: ./.venv/bin/python -m pip install pyserial")

HEARTBEAT_S = 2.0          # must stay well under the firmware's BRIDGE_TIMEOUT_MS (20 s)
POST_TIMEOUT_S = 4.0       # must stay under the firmware's BRIDGE_REPLY_TIMEOUT_MS (6 s)


def find_port() -> str | None:
    for pat in ("/dev/ttyUSB*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[0]
    return None


def relay(server: str, body: str) -> dict:
    """POST one batch to /ingest; wrap the reply for the firmware. code 0 means
    'server unreachable' — the firmware keeps the records buffered and retries."""
    try:
        r = requests.post(f"{server}/ingest", data=body.encode(),
                          headers={"Content-Type": "application/json"},
                          timeout=POST_TIMEOUT_S)
        try:
            rbody = r.json()
        except ValueError:
            rbody = {}
        return {"bridge": "ack", "code": r.status_code, "body": rbody,
                "server_time": int(time.time())}
    except requests.RequestException as e:
        print(f"[bridge] POST failed: {e}", flush=True)
        return {"bridge": "ack", "code": 0, "body": {}}


def pump(ser: serial.Serial, server: str) -> None:
    """One serial session: heartbeat + relay until the port dies."""
    last_hb = 0.0
    buf = b""
    while True:
        now = time.time()
        if now - last_hb >= HEARTBEAT_S:
            ser.write((json.dumps({"bridge": "hb", "server_time": int(now)}) + "\n").encode())
            last_hb = now
        buf += ser.read(4096)               # returns after `timeout` if idle
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf-8", "replace").rstrip("\r")
            if txt.startswith("#SYNC# "):
                ack = relay(server, txt[len("#SYNC# "):])
                ser.write((json.dumps(ack) + "\n").encode())
            elif txt:
                print(txt, flush=True)      # firmware log line — echo like a monitor


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="serial device (default: first /dev/ttyUSB*|ttyACM*)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--server", default="http://127.0.0.1:8000")
    args = ap.parse_args()

    while True:                              # outer loop: survive unplugs/reboots
        port = args.port or find_port()
        if not port:
            print("[bridge] no serial port found — waiting", flush=True)
            time.sleep(2)
            continue
        try:
            with serial.Serial(port, args.baud, timeout=0.2) as ser:
                print(f"[bridge] {port} @ {args.baud} → {args.server}/ingest", flush=True)
                pump(ser, args.server)
        except serial.SerialException as e:
            print(f"[bridge] serial error: {e} — retrying in 2 s", flush=True)
            time.sleep(2)
        except KeyboardInterrupt:
            print("\n[bridge] stopped — station will fall back to WiFi/power-saving", flush=True)
            return


if __name__ == "__main__":
    main()
