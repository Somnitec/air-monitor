# Firmware FIFO Ring Buffer, Duty-Cycled Sync & Testing Mode — Design

- **Date:** 2026-06-26
- **Status:** Approved (awaiting spec review)
- **Scope:** ESP32 firmware storage + sync layer, plus matching PC server / dashboard changes for mode display and control.
- **Out of scope:** CPU light/deep-sleep power optimization (separate task, gated on the power-budget unknowns in DESIGN.md), adaptive/rapid sampling (Phase 2).

---

## 1. Problem

The current firmware appends every reading to `/queue.ndjson` on LittleFS as a fat JSON line (~300–500 B) and tracks an acknowledged byte cursor. Space is reclaimed (`queueCompactIfDone`) **only** when the server has acked the entire file **and** the file has grown past 64 KB.

While the device is disconnected, nothing is acked, so nothing is ever reclaimed. The filesystem fills in roughly **2 days**, after which `queueAppendLine` fails and **new records are silently dropped**. There is no FIFO behavior — the oldest data is kept and the newest is lost — and JSON is far larger than necessary.

We want:

1. **FIFO ring storage** — when full, drop the **oldest** record to make room for the newest, so the device always retains the most recent window of data.
2. **Maximum on-device capacity** — store as many readings as the 4 MB flash allows.
3. **Duty-cycled sync** — keep WiFi off most of the time; attempt to offload on a timer so the radio is on only briefly.
4. **A real-time testing mode** — enterable shortly after power-on via a dashboard button, with the dashboard showing which mode each device is in.

## 2. Decisions (resolved during brainstorming)

| Question | Decision |
|----------|----------|
| Storage medium | Flash only (4 MB); no SD card |
| Overflow policy | Drop oldest (true FIFO ring) |
| Record format | Compact packed binary on-device |
| Capacity target / partitioning | Keep OTA; use existing default partition (~1.4 MB data ≈ ~2 weeks). No partition surgery. |
| Sync scheduling | Include duty-cycling now: WiFi off between syncs; **attempt offload every 15 min**, plus a buffer-threshold early-trigger |
| Testing-mode entry | Dashboard button, actionable during a post-boot WiFi window |
| Testing-mode cadence | Live sync, same 60 s sampling (WiFi stays on, each record pushed immediately) |
| Testing-mode exit | Dashboard "Stop" command |
| Wire format to PC | Stays JSON (binary is on-device only); derived values recomputed at sync time so the PC contract is preserved except for the new mode envelope |

## 3. Architecture overview

Three new/changed concerns, each with a clear boundary:

```
  sampling loop (firmware.cpp)
        │  buildRecord() -> Record (packed struct)
        ▼
  record.{h,cpp}      pack / unpack / record_to_json (re-derives Rs, soil%, bat V/%)
        │
        ▼
  ringstore.{h,cpp}   fixed-size binary FIFO ring on LittleFS (/ring.bin + /ring.meta)
        │  drain unsynced records as JSON batches
        ▼
  sync session (firmware.cpp)   WiFi on -> POST envelope -> apply server command -> WiFi off
        │  HTTP JSON
        ▼
  PC server (pc/server.py)      parse envelope, store per-device mode/last_seen/buffered,
        │                       return server_time + optional set_mode command
        ▼
  dashboard                     mode badge (TESTING/NORMAL/OFFLINE) + buffered count + Enter/Exit buttons
```

### Module boundaries

- **`record` module** — owns the on-device binary record format. Knows nothing about storage or WiFi.
  - `record_pack(...)` — fill a packed `Record` from live sensor values.
  - `record_to_json(const Record&, JsonDocument&)` — expand a packed record back to the existing JSON field set, re-deriving `co_rs`/`hcho_rs`, `soil_pct`, `bat_v`/`bat_pct` from the same config constants used today, so the PC receives an unchanged record shape.
  - A single `RECORD_SCHEMA_VERSION` constant; bump on any layout change.
- **`ringstore` module** — owns `/ring.bin` and `/ring.meta`. Knows nothing about sensors or JSON.
  - `ring_begin()` — mount, validate/recover metadata, preallocate `/ring.bin` if absent.
  - `ring_push(const Record&)` — write to `head` slot; advance `head_seq`; on full, advance `tail_seq` (drop oldest).
  - `ring_peek_unsynced(Record* out, size_t maxN, uint32_t& firstSeq)` — read up to `maxN` records from `synced_seq` forward.
  - `ring_mark_synced(uint32_t throughSeq)` — advance `synced_seq` after a server ack.
  - `ring_stats()` — `{ count, unsynced, capacity }`.
  - Host-testable (native env) with a LittleFS stub / file shim.
- **`firmware.cpp`** — orchestration only: sampling cadence, mode state machine, sync-session trigger logic, WiFi on/off.

## 4. On-device storage: binary FIFO ring

### 4.1 Record layout (~72 bytes)

Fixed-size, packed. All multi-byte fields little-endian (native ESP32 order; PC decodes accordingly). Quantization scales chosen so resolution far exceeds each sensor's accuracy.

| Field | Type | Scale / encoding | Bytes |
|-------|------|------------------|-------|
| `seq` | u32 | monotonic record number | 4 |
| `ts` | u32 | unix epoch (UTC); 0/`< EPOCH_VALID_AFTER` = clock unsynced | 4 |
| `up_ms` | u32 | `millis()` at capture (PC back-fills ts) | 4 |
| `flags` | u16 | bit0 `ts_ok`, bit1 `bat_cal`, bit2 `noise_clip`, bits3–11 present flags (sen66, bh1750, bme, adxl, co, hcho, soil, battery, mic) | 2 |
| `boot` | u16 | low 16 bits of per-boot id (grouping; collision-tolerant) | 2 |
| `pm1,pm25,pm4,pm10` | u16×4 | µg/m³ ×10 | 8 |
| `co2` | u16 | ppm | 2 |
| `voc,nox` | u16×2 | index ×1 | 4 |
| `temp` | i16 | °C ×100 | 2 |
| `rh` | u16 | %RH ×100 | 2 |
| `lux` | u16 | lux ×1 (0–65535) | 2 |
| `pressure` | u16 | hPa ×10 | 2 |
| `bme_temp` | i16 | °C ×100 | 2 |
| `bme_rh` | u16 | %RH ×100 | 2 |
| `rumble_rms` | u16 | m/s² ×1000 | 2 |
| `rumble_peak` | u16 | m/s² ×1000 | 2 |
| `accel_mag` | u16 | m/s² ×100 | 2 |
| `co_mv` | u16 | mV | 2 |
| `hcho_mv` | u16 | mV | 2 |
| `soil_mv` | u16 | mV | 2 |
| `bat_raw_mv` | u16 | mV | 2 |
| `noise_dba` | i16 | dB(A) ×10 | 2 |
| `noise_spl` | i16 | dB ×10 | 2 |
| `noise_dbfs` | i16 | dBFS ×10 | 2 |
| `bands[9]` | u8×9 | dB(A) rounded, clamped 0–255 | 9 |
| **Total** | | | **~71–72** |

Notes:
- Absent sensors store a sentinel (0 for unsigned, with the present-bit clear) and are omitted from the JSON on unpack.
- `co_rs`/`hcho_rs` are **not stored** — recomputed on unpack from `co_mv`/`hcho_mv` and `GAS_*_RL_OHMS` (the same `gasRs()` math). Likewise `soil_pct` from `soil_mv` + soil calibration, and `bat_v`/`bat_pct` from `bat_raw_mv` + `BAT_DIVIDER_FACTOR`/`BAT_*_V` when `bat_cal` is set. This keeps the PC-facing JSON identical to today's.
- `RECORD_SIZE` is fixed at compile time; a `static_assert` guards the struct size.

### 4.2 Ring file and capacity

- **`/ring.bin`**: a single file preallocated to `RING_CAPACITY * RECORD_SIZE` bytes, sized to fill the LittleFS data partition with headroom (target ~1.3 MB).
- **Slots** indexed `0 … RING_CAPACITY-1`. The slot for a record is `seq % RING_CAPACITY`. Records are written in place via `seek(slot * RECORD_SIZE)`.
- **Capacity:** ~1.3 MB / 72 B ≈ **~18,000 records ≈ 12–14 days** at 60 s. (7× today, OTA preserved.)
- **Write amplification / wear:** each ~72 B write dirties one ~4 KB LittleFS block; a full ring cycle (~2 weeks) erases each block only a few hundred times — centuries from the ~100 k-cycle flash limit. Acceptable; we prioritize durability (write each record immediately) over batching.

### 4.3 Metadata and FIFO pointer semantics

State is three monotonic sequence numbers:
- `head_seq` — next seq to write.
- `tail_seq` — oldest seq still stored (`head_seq - tail_seq ≤ RING_CAPACITY`).
- `synced_seq` — boundary up to which the server has acked (`tail_seq ≤ synced_seq ≤ head_seq`).

Operations:
- **push:** write record at `head_seq % CAP`; `head_seq++`. If `head_seq - tail_seq > RING_CAPACITY`, advance `tail_seq` (overwriting the oldest). If the overwritten slot was unsynced (`tail_seq` passes `synced_seq`), bump `synced_seq = tail_seq` — that data is lost by the drop-oldest policy, which is the accepted overflow behavior.
- **drain:** read `[synced_seq, head_seq)` in batches; on HTTP 200 set `synced_seq = throughSeq`.

Persistence:
- **`/ring.meta`**: `{ magic, schema_ver, record_size, capacity, head_seq, tail_seq, synced_seq, write_counter, crc }`.
- **Double-buffered**: alternately written to two slots (A/B) each carrying `write_counter` + CRC. On boot, pick the highest `write_counter` with a valid CRC. A torn write during power loss falls back to the previous good copy.
- **Ordering:** record data is written **before** the metadata update. A power cut between them leaves metadata one record behind; that slot is simply re-used next boot. **Worst-case loss on power cut: one in-flight record.**

### 4.4 Migration

On boot, if legacy `/queue.ndjson`/`/cursor.txt` exist they are removed (best effort) — Phase 1 has no deployed data worth migrating. If `/ring.meta` is missing or its `schema_ver` differs, reinitialize the ring (preallocate `/ring.bin`, zero the pointers).

## 5. Duty-cycled sync

Default mode is **`normal`**: WiFi is off during sampling. A **sync session** runs when either trigger fires:

- **Timer:** `SYNC_ATTEMPT_INTERVAL_MS` = **15 min** since the last attempt (primary trigger).
- **Threshold:** `unsynced ≥ SYNC_THRESHOLD_RECORDS` (e.g. 120) — an early trigger for bursts; rarely hit at 60 s baseline (~15 records per 15 min), but matters once rapid sampling exists.

A sync session:
1. WiFi on (`WIFI_STA`), `wifiMulti.run(...)`.
2. If connected: `syncTimeIfNeeded()` (NTP), `discoverServer()` if no host cached.
3. Drain the ring: `ring_peek_unsynced` → `record_to_json` per record → POST envelope batches (reusing `SYNC_BATCH_MAX` / `SYNC_BATCH_MAX_BYTES`); `ring_mark_synced` on each 200.
4. Apply any server command (§6).
5. WiFi off (`WIFI_MODE_NULL`).

If the connection fails, end the session and retry on the next trigger (no busy-retry). Backoff is unnecessary given the 15-min cadence.

## 6. Modes, command channel, and dashboard

### 6.1 Modes

- **`normal`** — §5 duty-cycled behavior. WiFi off between sessions.
- **`testing`** — WiFi stays on; each record is pushed live immediately after the 60 s sample (no batching, no 15-min wait). Sampling cadence stays 60 s.

The device starts every boot in a **boot window** (`BOOT_WINDOW_MS`, e.g. 5 min) with WiFi on and contacting the server frequently. This is the window in which a dashboard "Enter testing" press takes effect **immediately**. If no testing command arrives before the window expires, the device transitions to `normal` (WiFi off).

In `normal` mode the device is offline between 15-min sessions, so a dashboard button press is honored at the **next session** (≤ 15 min). Power-cycling restarts the boot window for an immediate switch.

Mode is **not persisted** across reboots — every power-on starts in the boot window and defaults to `normal`. Predictable and simple.

### 6.2 Command channel (rides the existing HTTP sync)

- **Device → server** (`POST /ingest`) — envelope replacing the bare array:
  ```json
  {
    "dev": "air-monitor-01",
    "boot": 123456789,
    "fw": "phase1",
    "mode": "normal",
    "buffered": 42,
    "records": [ { ...record... }, ... ]
  }
  ```
  `buffered` = unsynced count; `mode` = the device's current mode (lets the dashboard confirm a switch took effect).
- **Server → device** (200 reply) — adds an optional command:
  ```json
  { "server_time": 1719400000, "command": { "set_mode": "testing" } }
  ```
  `server_time` is still adopted when the clock is unsynced (unchanged). `command` is applied then cleared server-side once the device echoes the new `mode`.

### 6.3 PC server (`pc/server.py`)

- Accept the envelope (and, for resilience, still accept a bare array as legacy).
- Store records as today.
- Maintain per-device state: `mode`, `last_seen`, `buffered`, and a `pending_command` set by the dashboard.
- Return `server_time` + any `pending_command` as `set_mode`; clear it once the device's reported `mode` matches.
- Expose this device state on the existing query API / WebSocket for the dashboard, plus an endpoint for the dashboard buttons to set `pending_command`.

### 6.4 Dashboard

- Per-device **badge**: `TESTING` (mode=testing), `NORMAL` (mode=normal), or `OFFLINE` (`last_seen` older than a staleness window, e.g. > 2× the 15-min interval).
- Show **buffered** record count and last-seen time.
- **Enter testing** / **Stop** buttons that set the device's `pending_command`. While the device is offline in normal mode, surface that the command is **pending** until the next session.

## 7. Configuration additions (`config.h`)

- `RECORD_SCHEMA_VERSION`, `RECORD_SIZE` (static_assert guarded).
- `RING_PATH "/ring.bin"`, `RING_META_PATH "/ring.meta"`, `RING_CAPACITY` (sized to partition).
- `SYNC_ATTEMPT_INTERVAL_MS` = 15 min, `SYNC_THRESHOLD_RECORDS` = 120.
- `BOOT_WINDOW_MS` = 5 min.
- Reuse `SYNC_BATCH_MAX`, `SYNC_BATCH_MAX_BYTES`.
- Retire `QUEUE_*` constants once the NDJSON path is removed.

## 8. Testing

**Host-side unit tests (PlatformIO native env)** for the pure modules:
- `record`: pack→unpack round-trip for every field; quantization bounds/clamping; absent-sensor sentinels; `record_to_json` re-derivation of Rs/soil%/bat V matches the legacy formulas; `static_assert` on `RECORD_SIZE`.
- `ringstore`: append + read-back; wrap-around at capacity; **drop-oldest** overflow (oldest seq lost, newest retained); `synced_seq` advance and that acked records are never re-sent; metadata double-buffer recovery (corrupt the newer copy → falls back); torn last-record tolerance.

**Integration / on-device:**
- Fill the ring past capacity while "disconnected"; confirm the most recent ~18 k records survive and oldest roll off.
- Power-cut during a push → at most one record lost, pointers consistent on reboot.
- 15-min duty cycle: WiFi on only during sessions; data drains; `synced_seq` advances.
- Testing mode: enter via dashboard during boot window → live records within seconds; Stop → returns to normal.
- Dashboard reflects mode/buffered/last-seen and the pending-command state.

**Data quality (unchanged goals):** no duplicate `ts` on the PC (server `UNIQUE` constraint), values within physical bounds after unpack.

## 9. Risks / open items

- **Quantization scales** must bound every sensor's real range (PM up to ~1000 µg/m³ ×10 = 10 000 fits u16; lux clamps at 65 535; pressure 300–1100 hPa ×10 fits). Verify each against `docs/SENSORS.md` during implementation.
- **Boot-window cost:** ~5 min of WiFi on every power-on. Acceptable; revisit with the power budget.
- **Normal-mode command latency:** up to 15 min to enter testing unless power-cycled. Documented in the dashboard UI.
- **PC/dashboard changes** are required for mode display/control (envelope parsing, per-device state, buttons) — in scope here.
