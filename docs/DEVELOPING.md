# Developing & Testing the Air Monitor

Practical guide for building, testing, flashing, and continuing work on the
firmware + PC server. Start here if you're picking up the project.

Design references:
- System design: [`DESIGN.md`](../DESIGN.md)
- FIFO ring / duty-cycle / testing-mode feature:
  spec [`docs/superpowers/specs/2026-06-26-firmware-fifo-ring-buffer-design.md`](superpowers/specs/2026-06-26-firmware-fifo-ring-buffer-design.md),
  plan [`docs/superpowers/plans/2026-06-26-firmware-fifo-ring-buffer.md`](superpowers/plans/2026-06-26-firmware-fifo-ring-buffer.md)

---

## Repository layout

```
firmware/                 ESP32 (PlatformIO, Arduino framework)
  include/
    config.h              pin map, calibration, ring + sync + mode constants
    record.h              binary Record struct, flags, quantization, RecordFields
    ringlogic.h           pure FIFO pointer math (host-testable)
    ringstore.h           LittleFS-backed ring API
    mic.h / accel.h       sensor capture result structs
  src/
    firmware.cpp          main: sample -> pack -> ring; duty-cycled sync; modes
    record.cpp            pack / unpack / record_to_json (re-derives Rs, soil%, bat V)
    ringlogic.cpp         FIFO pointer math
    ringstore.cpp         ring file + double-buffered CRC metadata
    mic.cpp / accel.cpp   sensor capture
  secrets.h               WiFi creds + sync host (gitignored; copy secrets.example.h)
  test/
    test_record/          Unity tests (native)
    test_ringlogic/       Unity tests (native)
  platformio.ini          envs: esp32_phase1[_ota], esp32_tester, esp32_sensor_test, native

server/                   PC collector + dashboard (FastAPI + SQLite)
  server.py               /ingest (envelope), /api/*, /ws, dashboard, mDNS advert
  static/index.html       Plotly dashboard + device mode badge/controls
  test_server.py          pytest for ingest + testing-mode command flow
```

---

## How the storage + sync works (the short version)

- Each minute the firmware reads every present sensor, packs it into a **~71-byte
  binary `Record`**, and pushes it into a **fixed-size FIFO ring** on flash
  (`/ring.bin`, `RING_CAPACITY` slots ≈ ~12 days at 60 s). When the ring is full
  it **overwrites the oldest** record (drop-oldest).
- Ring pointers (`head`/`tail`/`synced` sequence numbers) live in a tiny
  **double-buffered, CRC'd** `/ring.meta`. Data is written before metadata, so a
  power cut loses at most the single in-flight record.
- **Normal mode:** WiFi is **off** between syncs. The device powers the radio up
  and drains the un-synced tail to the server **every 15 min** (or sooner if
  `SYNC_THRESHOLD_RECORDS` accumulate), then turns WiFi off.
- **Testing mode:** WiFi stays on and every record is POSTed live.
- The on-flash format is binary, but the **wire format stays JSON** — `record_to_json`
  expands a record back to the original field names and re-derives `co_rs`,
  `hcho_rs`, `soil_pct`, `bat_v`, `bat_pct`. The server and dashboard are unchanged
  except for the new device-mode envelope/badge.

### Command channel (mode control)
- Device → server POST `/ingest` is an envelope:
  `{dev, boot, fw, mode, buffered, records:[...]}`.
- Server → device 200 reply may carry `{server_time, command:{set_mode:"testing"|"normal"}}`.
- The dashboard's Enter/Stop-testing buttons set a **pending command** that the
  server returns on the device's next contact; it clears once the device echoes
  the new mode.

---

## Toolchain setup

You need **PlatformIO** (firmware build + native unit tests) and a few **Python**
packages (server + its tests). Neither needs the physical board for the host-side
tests.

### Option A — pipx / global PlatformIO
```bash
pipx install platformio        # or: pip install --user platformio
```

### Option B — a local virtualenv (works around PEP 668 "externally managed")
```bash
python3 -m venv .venv
# if `ensurepip` is missing on your distro, bootstrap pip once:
#   curl -sSL https://bootstrap.pypa.io/get-pip.py | .venv/bin/python
.venv/bin/pip install platformio fastapi uvicorn httpx pytest
# then prefix commands with .venv/bin/ (e.g. .venv/bin/pio, .venv/bin/pytest)
```

The first `pio run -e esp32_phase1` downloads the ESP32 (xtensa) toolchain; the
first `pio test -e native` downloads the host `native` platform + Unity +
ArduinoJson. All are fetched automatically (needs network, one-time).

---

## Running the tests

### Firmware unit tests (host / no board)
Pure modules (`record`, `ringlogic`) are tested on the host with Unity:
```bash
cd firmware
pio test -e native
```
Expected: `test_record` and `test_ringlogic` PASS (10 test cases). These cover
quantization round-trips, JSON expansion + derived values, FIFO wrap-around,
drop-oldest overflow, and the synced-pointer logic.

### Firmware compile check (host / no board)
```bash
cd firmware
pio run -e esp32_phase1        # must link cleanly (SUCCESS)
```

### Server tests (host / no board)
```bash
cd server
AIRMON_DB=:memory: pytest test_server.py -v
```
Covers envelope ingest, legacy list/dict compatibility, the pending-command
round-trip, and mode validation.

---

## Building & flashing the firmware

1. Create `secrets.h` from `secrets.example.h` in the repo root (WiFi
   networks, sync host fallback, calibration overrides). It is gitignored.
2. Flash over USB:
   ```bash
   cd firmware
   pio run -e esp32_phase1 -t upload
   pio device monitor            # 115200 baud
   ```
3. Or flash over WiFi (OTA) — set the device IP in the `[env:esp32_phase1_ota]`
   `upload_port` in `platformio.ini`, then:
   ```bash
   pio run -e esp32_phase1_ota -t upload
   ```

On the serial monitor you should see, on first boot, `[ring] preallocated ...`
(then `[ring] resumed ...` on later boots) and a `[rec] seq=… buffered=… mode=normal`
line every 60 s.

---

## Running the PC server + dashboard

```bash
cd server
pip install -r requirements.txt          # fastapi, uvicorn, zeroconf, ...
python server.py                          # serves http://0.0.0.0:8000
# DB path override: AIRMON_DB=/media/usb/air-monitor.db python server.py
```
Open `http://<server-ip>:8000/`. The ESP32 finds the server by mDNS
(`_airmon._tcp`, host `airmon-server.local`); if zeroconf isn't available it
falls back to `SYNC_HOST`/`SYNC_PORT` from `secrets.h`.

---

## End-to-end testing-mode workflow (with hardware)

1. Power on the device. For the first **`BOOT_WINDOW_MS`** (default 5 min) WiFi is
   up and it polls the server every ~5 s — this is the window where a dashboard
   command lands immediately.
2. On the dashboard, the **device badge** shows `NORMAL` (or `OFFLINE` if not seen
   recently) plus the buffered-record count.
3. Click **Enter testing mode**. Within a few seconds the serial monitor prints
   `[mode] -> TESTING (server)` and readings start arriving live (badge → `TESTING`).
4. Click **Stop testing mode** → `[mode] -> NORMAL (server)`; the device drains and
   drops WiFi until the next 15-min cycle.
5. In normal mode the device is offline most of the time, so a button press is
   **pending** until the next sync (≤ 15 min). Power-cycling restarts the boot
   window for an instant switch.

### Verifying FIFO behavior without waiting days
- Drop-oldest overflow is covered deterministically by the
  `test_ringlogic` unit test (`test_overflow_drops_oldest`).
- For a live check: stop the server, watch `buffered` climb on the monitor and the
  device retry on the ~15-min cadence (not every loop); restart the server and
  confirm the backlog drains oldest-first.

---

## Tuning knobs (`firmware/include/config.h`)

| Constant | Default | Meaning |
|---|---|---|
| `RING_CAPACITY` | 18000 | ring slots (× ~71 B ≈ 1.28 MB ≈ ~12.5 days @ 60 s). Keep under the LittleFS data partition size (~1.44 MB default). |
| `SAMPLE_BASELINE_MS` | 60000 | sampling cadence |
| `SYNC_ATTEMPT_INTERVAL_MS` | 15 min | normal-mode offload cadence |
| `SYNC_THRESHOLD_RECORDS` | 120 | early-trigger sync when this many are buffered |
| `BOOT_WINDOW_MS` | 5 min | WiFi-on window after power-on for immediate testing entry |
| `SYNC_BATCH_MAX` / `_BYTES` | 20 / 4096 | records (or bytes) per HTTP POST |

If you change the `Record` layout, bump `RECORD_SCHEMA_VERSION` in `record.h`
(a mismatched `/ring.meta` causes the ring to reinitialize on boot) and re-run
`pio test -e native`.

---

## Known limitations / next steps

- **Power:** only the WiFi radio is duty-cycled; the CPU stays awake between
  samples. Deep/light-sleep is future work (complicated by the SEN66 continuous
  measurement and the RAM-resident ring metadata) — gated on the power-budget
  measurements in `DESIGN.md`.
- **On-hardware field verification** of the boot window, live testing round-trip,
  and real disconnected drop-oldest still needs a board run.
- **Multi-device dashboard:** the badge currently assumes a single device
  (`devs[0]`). Generalize if you add more nodes.
