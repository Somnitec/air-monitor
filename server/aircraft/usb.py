"""Detect whether an RTL-SDR dongle is physically plugged in.

Independent of whether readsb is running: the dashboard shows "SDR connected" vs
"no SDR" so a missing/unplugged dongle is distinguishable from a dead decoder or
simply-no-traffic. Reads sysfs directly (no lsusb dependency); on non-Linux or if
sysfs isn't readable it returns unknown rather than a false negative.
"""
from __future__ import annotations

from pathlib import Path

# Realtek RTL2832U-based SDRs. Vendor 0bda; product 2832 (generic) / 2838 (RTL-SDR
# Blog incl. V4). The dongle enumerates under one of these regardless of driver.
_RTLSDR_IDS = {("0bda", "2832"), ("0bda", "2838")}
_USB_ROOT = Path("/sys/bus/usb/devices")


def rtlsdr_status() -> dict:
    """Return {'connected': bool|None, 'name': str|None}. `connected` is None when
    USB can't be inspected (non-Linux / no sysfs), so the UI can show 'unknown'."""
    if not _USB_ROOT.is_dir():
        return {"connected": None, "name": None}
    try:
        for dev in _USB_ROOT.iterdir():
            vid = (dev / "idVendor")
            pid = (dev / "idProduct")
            if not (vid.is_file() and pid.is_file()):
                continue
            if (vid.read_text().strip(), pid.read_text().strip()) in _RTLSDR_IDS:
                prod = dev / "product"
                name = prod.read_text().strip() if prod.is_file() else "RTL-SDR"
                return {"connected": True, "name": name}
    except OSError:
        return {"connected": None, "name": None}
    return {"connected": False, "name": None}
