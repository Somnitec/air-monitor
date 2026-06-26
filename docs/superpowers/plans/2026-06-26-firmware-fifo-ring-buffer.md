# Firmware FIFO Ring Buffer, Duty-Cycled Sync & Testing Mode — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fat-JSON append-only queue with a dense binary FIFO ring buffer on flash (~2 weeks of data, drop-oldest on overflow), make the ESP32 sync on a 15-minute duty cycle with WiFi off in between, and add a dashboard-controlled real-time testing mode with a per-device mode badge.

**Architecture:** Three on-device modules with clean boundaries — `record` (pure pack/unpack of a ~71 B binary record + JSON expansion), `ringlogic` (pure FIFO pointer math, host-testable), and `ringstore` (thin LittleFS glue binding the two). `firmware.cpp` orchestrates sampling → push, the duty-cycled sync session, and a normal/testing mode state machine driven by commands returned in the existing `/ingest` HTTP reply. The PC server gains per-device state + a pending-command channel; the dashboard shows a mode badge and Enter/Exit-testing buttons. The JSON wire format is preserved (derived Rs/soil%/battery values are recomputed at sync time), so storage is the only thing that goes binary.

**Tech Stack:** C++17 / Arduino-ESP32 / PlatformIO (LittleFS, ArduinoJson, Unity for native unit tests), Python / FastAPI / SQLite, vanilla JS + Plotly dashboard.

---

## Reference: current behavior being replaced

- `firmware/src/firmware.cpp` — `buildRecord()` (lines ~225–315) emits a JSON object; `queueAppendLine()`/`syncQueue()`/`queueCompactIfDone()` implement the NDJSON queue; `loop()` samples every `SAMPLE_BASELINE_MS` and drains whenever WiFi is up.
- `firmware/include/config.h` — `QUEUE_*`, `SYNC_BATCH_*`, `SAMPLE_BASELINE_MS`, all sensor calibration constants (`GAS_*_RL_OHMS`, `GAS_VCC_MV`, `SOIL_DRY_MV`, `SOIL_WET_MV`, `BAT_*`).
- `server/server.py` — `/ingest` accepts a single dict or a list; `_dedupe_insert`, `_backfill_times`, `Hub` WS fan-out.
- `server/static/index.html` — Plotly dashboard, WS `reading`/`event` messages.

The JSON field names produced today (and consumed by the dashboard `META` map) **must stay identical**: `ts, ts_ok, dev, up_ms, boot, pm1, pm25, pm4, pm10, co2, voc, nox, temp, rh, lux, pressure_hpa, bme_temp, bme_rh, rumble, rumble_peak, accel_mag, co_mv, co_rs, hcho_mv, hcho_rs, soil_mv, soil_pct, bat_raw_mv, bat_cal, bat_v, bat_pct, noise_dba, noise_spl, noise_dbfs, noise_clip, noise_bands`.

---

## File Structure

**New (firmware):**
- `firmware/include/record.h` — binary `Record` struct, `RecordFields` plain inputs, flag bits, quantization scales, API.
- `firmware/src/record.cpp` — `record_pack`, `record_unpack`, `record_to_json`.
- `firmware/include/ringlogic.h` — pure FIFO pointer math (`RingLogic` + functions).
- `firmware/src/ringlogic.cpp` — implementation.
- `firmware/include/ringstore.h` — LittleFS-backed ring API.
- `firmware/src/ringstore.cpp` — file + double-buffered metadata glue.
- `firmware/test/test_record/test_record.cpp` — Unity tests (native).
- `firmware/test/test_ringlogic/test_ringlogic.cpp` — Unity tests (native).

**Modified:**
- `firmware/platformio.ini` — add `[env:native]` test env; add `record.cpp`+`ringlogic.cpp`+`ringstore.cpp` to the phase1 `build_src_filter`.
- `firmware/include/config.h` — add ring + duty-cycle + mode constants; mark `QUEUE_*` retired.
- `firmware/src/firmware.cpp` — swap queue for ringstore; add mode state machine + sync session.
- `server/server.py` — envelope ingest, per-device state, pending-command channel, `/api/devices` + `/api/device/{dev}/mode`.
- `server/static/index.html` — device mode badge + Enter/Exit testing buttons.
- `server/test_server.py` (new) — pytest for envelope ingest + command flow.

---

## Task 1: Native test environment

**Files:**
- Modify: `firmware/platformio.ini`

- [ ] **Step 1: Add a native unit-test environment**

Append to `firmware/platformio.ini`:

```ini
; ===========================================================================
; Native host unit tests for the pure modules (record, ringlogic).
; Run with:  pio test -e native
; Only Arduino-free sources compile here.
; ===========================================================================
[env:native]
platform = native
build_flags = -std=gnu++17 -DUNIT_TEST
build_src_filter = +<record.cpp> +<ringlogic.cpp>
lib_deps = bblanchon/ArduinoJson @ ^7.2.0
test_framework = unity
```

- [ ] **Step 2: Verify the env is recognized**

Run: `cd firmware && pio test -e native --list-tests || true`
Expected: PlatformIO lists the `native` env (no tests yet is fine; it must not error on the env itself).

- [ ] **Step 3: Commit**

```bash
git add firmware/platformio.ini
git commit -m "build: add native unit-test environment for pure firmware modules"
```

---

## Task 2: `record` module — header + flag/scale definitions

**Files:**
- Create: `firmware/include/record.h`

- [ ] **Step 1: Write the record header**

Create `firmware/include/record.h`:

```cpp
#pragma once
// Dense binary record for the on-device FIFO ring. ~71 bytes vs ~400 bytes of
// JSON. Stored verbatim in the ring; expanded back to the existing JSON shape
// only at sync time (record_to_json), so the PC contract is unchanged.
//
// Quantization scales are chosen so resolution far exceeds each sensor's real
// accuracy. Absent sensors store 0 and clear their present bit.

#include <stdint.h>

class JsonDocument;   // fwd-decl; record_to_json defined in record.cpp

static constexpr uint8_t  RECORD_SCHEMA_VERSION = 1;
static constexpr int      REC_NBANDS = 9;          // must match MIC_NBANDS

// ---- flag bits ----
enum : uint16_t {
    FLAG_TS_OK       = 1u << 0,
    FLAG_BAT_CAL     = 1u << 1,
    FLAG_NOISE_CLIP  = 1u << 2,
    PRESENT_SEN66    = 1u << 3,
    PRESENT_BH1750   = 1u << 4,
    PRESENT_BME      = 1u << 5,
    PRESENT_ADXL     = 1u << 6,
    PRESENT_CO       = 1u << 7,
    PRESENT_HCHO     = 1u << 8,
    PRESENT_SOIL     = 1u << 9,
    PRESENT_BATTERY  = 1u << 10,
    PRESENT_MIC      = 1u << 11,
};

#pragma pack(push, 1)
struct Record {
    uint32_t seq;          // monotonic record number
    uint32_t ts;           // unix epoch (UTC); < EPOCH_VALID_AFTER => clock unsynced
    uint32_t up_ms;        // millis() at capture
    uint16_t flags;        // FLAG_* | PRESENT_*
    uint16_t boot;         // low 16 bits of per-boot id

    uint16_t pm1, pm25, pm4, pm10;  // ug/m3 x10
    uint16_t co2;                   // ppm
    uint16_t voc, nox;              // index x1
    int16_t  temp;                  // C x100
    uint16_t rh;                    // %RH x100

    uint16_t lux;                   // lux x1
    uint16_t pressure;              // hPa x10
    int16_t  bme_temp;              // C x100
    uint16_t bme_rh;                // %RH x100

    uint16_t rumble_rms;            // m/s^2 x1000
    uint16_t rumble_peak;           // m/s^2 x1000
    uint16_t accel_mag;             // m/s^2 x100

    uint16_t co_mv;                 // mV
    uint16_t hcho_mv;               // mV
    uint16_t soil_mv;               // mV
    uint16_t bat_raw_mv;            // mV

    int16_t  noise_dba;             // dB(A) x10
    int16_t  noise_spl;             // dB x10
    int16_t  noise_dbfs;            // dBFS x10
    uint8_t  bands[REC_NBANDS];     // dB(A) rounded, clamped 0..255
};
#pragma pack(pop)

static constexpr unsigned RECORD_SIZE = 71;
static_assert(sizeof(Record) == RECORD_SIZE, "Record must be tightly packed to 71 bytes");

// Plain (Arduino-free) inputs to pack. firmware.cpp fills this from live sensors.
struct RecordFields {
    uint32_t seq = 0, ts = 0, up_ms = 0;
    uint16_t boot = 0;
    bool ts_ok = false, bat_cal = false, noise_clip = false;

    bool  has_sen66 = false;
    float pm1 = 0, pm25 = 0, pm4 = 0, pm10 = 0, voc = 0, nox = 0, temp = 0, rh = 0;
    uint16_t co2 = 0;

    bool  has_bh1750 = false; float lux = 0;
    bool  has_bme = false;    float pressure = 0, bme_temp = 0, bme_rh = 0;
    bool  has_adxl = false;   float rumble_rms = 0, rumble_peak = 0, accel_mag = 0;
    bool  has_co = false;     uint16_t co_mv = 0;
    bool  has_hcho = false;   uint16_t hcho_mv = 0;
    bool  has_soil = false;   uint16_t soil_mv = 0;
    bool  has_battery = false; uint16_t bat_raw_mv = 0;
    bool  has_mic = false;    float noise_dba = 0, noise_spl = 0, noise_dbfs = 0;
    float bands[REC_NBANDS] = {0};
};

Record record_pack(const RecordFields& f);
void    record_unpack(const Record& r, RecordFields& out);   // inverse (lossy by quantization)

// Expand a record into the existing JSON field set, re-deriving co_rs/hcho_rs,
// soil_pct, bat_v/bat_pct from config constants. `doc` must be a JsonDocument.
void    record_to_json(const Record& r, JsonDocument& doc);
```

- [ ] **Step 2: Commit**

```bash
git add firmware/include/record.h
git commit -m "feat(record): binary record struct, flags, fields header"
```

---

## Task 3: `record` pack/unpack — failing test first

**Files:**
- Create: `firmware/test/test_record/test_record.cpp`

- [ ] **Step 1: Write the failing round-trip + quantization test**

Create `firmware/test/test_record/test_record.cpp`:

```cpp
#include <unity.h>
#include <math.h>
#include "record.h"

void setUp() {}
void tearDown() {}

static RecordFields sample() {
    RecordFields f;
    f.seq = 42; f.ts = 1719400000; f.up_ms = 123456; f.boot = 0xBEEF;
    f.ts_ok = true; f.bat_cal = true; f.noise_clip = false;
    f.has_sen66 = true;
    f.pm1 = 1.2f; f.pm25 = 3.4f; f.pm4 = 5.6f; f.pm10 = 7.8f;
    f.co2 = 812; f.voc = 101; f.nox = 3; f.temp = 21.37f; f.rh = 48.5f;
    f.has_bh1750 = true; f.lux = 333;
    f.has_bme = true; f.pressure = 1013.2f; f.bme_temp = 21.5f; f.bme_rh = 49.0f;
    f.has_adxl = true; f.rumble_rms = 0.123f; f.rumble_peak = 0.987f; f.accel_mag = 9.81f;
    f.has_co = true; f.co_mv = 410;
    f.has_hcho = true; f.hcho_mv = 222;
    f.has_soil = true; f.soil_mv = 1800;
    f.has_battery = true; f.bat_raw_mv = 2750;
    f.has_mic = true; f.noise_dba = 41.2f; f.noise_spl = 55.5f; f.noise_dbfs = -38.4f;
    for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = 20.0f + b;
    return f;
}

void test_size_is_71() { TEST_ASSERT_EQUAL_UINT(71, sizeof(Record)); }

void test_roundtrip_preserves_within_quantization() {
    RecordFields in = sample();
    Record r = record_pack(in);
    RecordFields out;
    record_unpack(r, out);

    TEST_ASSERT_EQUAL_UINT32(in.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT32(in.ts, out.ts);
    TEST_ASSERT_EQUAL_UINT16(in.boot, out.boot);
    TEST_ASSERT_TRUE(out.ts_ok);
    TEST_ASSERT_TRUE(out.bat_cal);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.pm25, out.pm25);   // x10 scale -> 0.1 resolution
    TEST_ASSERT_EQUAL_UINT16(in.co2, out.co2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, in.temp, out.temp);  // x100 -> 0.01 resolution
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.pressure, out.pressure);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, in.rumble_rms, out.rumble_rms);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.noise_dbfs, out.noise_dbfs);
    TEST_ASSERT_EQUAL_UINT16(in.bat_raw_mv, out.bat_raw_mv);
    TEST_ASSERT_FLOAT_WITHIN(0.6f, in.bands[5], out.bands[5]);  // u8 dB rounding
}

void test_absent_sensors_clear_present_bits() {
    RecordFields in;      // all has_* default false
    in.seq = 1; in.ts = 1719400000;
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_SEN66);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_MIC);
}

void test_quantization_clamps_high_values() {
    RecordFields in; in.has_sen66 = true; in.pm25 = 1e9f;   // absurd
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(65535, r.pm25);                // clamped, no wrap
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_size_is_71);
    RUN_TEST(test_roundtrip_preserves_within_quantization);
    RUN_TEST(test_absent_sensors_clear_present_bits);
    RUN_TEST(test_quantization_clamps_high_values);
    return UNITY_END();
}
```

- [ ] **Step 2: Run it to verify it fails to link**

Run: `cd firmware && pio test -e native -f test_record`
Expected: FAIL — `record_pack`/`record_unpack` undefined (record.cpp not written yet).

- [ ] **Step 3: Commit the failing test**

```bash
git add firmware/test/test_record/test_record.cpp
git commit -m "test(record): pack/unpack round-trip + quantization (failing)"
```

---

## Task 4: `record` pack/unpack/to_json — implementation

**Files:**
- Create: `firmware/src/record.cpp`

- [ ] **Step 1: Implement record.cpp**

Create `firmware/src/record.cpp`:

```cpp
#include "record.h"
#include "config.h"     // GAS_*_RL_OHMS, GAS_VCC_MV, SOIL_*_MV, BAT_*
#include <ArduinoJson.h>
#include <math.h>

// ---- quantization helpers ----
static uint16_t qU16(float v, float scale) {
    if (!(v > 0)) return 0;                  // also catches NaN
    float s = roundf(v * scale);
    if (s > 65535.0f) return 65535;
    return (uint16_t)s;
}
static int16_t qI16(float v, float scale) {
    float s = roundf(v * scale);
    if (s >  32767.0f) return  32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}
static uint8_t qU8(float v) {
    float s = roundf(v);
    if (s > 255.0f) return 255;
    if (s < 0.0f)   return 0;
    return (uint8_t)s;
}
static void setBit(uint16_t& flags, uint16_t bit, bool on) { if (on) flags |= bit; }

Record record_pack(const RecordFields& f) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.seq = f.seq; r.ts = f.ts; r.up_ms = f.up_ms; r.boot = f.boot;

    uint16_t flags = 0;
    setBit(flags, FLAG_TS_OK,      f.ts_ok);
    setBit(flags, FLAG_BAT_CAL,    f.bat_cal);
    setBit(flags, FLAG_NOISE_CLIP, f.noise_clip);
    setBit(flags, PRESENT_SEN66,   f.has_sen66);
    setBit(flags, PRESENT_BH1750,  f.has_bh1750);
    setBit(flags, PRESENT_BME,     f.has_bme);
    setBit(flags, PRESENT_ADXL,    f.has_adxl);
    setBit(flags, PRESENT_CO,      f.has_co);
    setBit(flags, PRESENT_HCHO,    f.has_hcho);
    setBit(flags, PRESENT_SOIL,    f.has_soil);
    setBit(flags, PRESENT_BATTERY, f.has_battery);
    setBit(flags, PRESENT_MIC,     f.has_mic);
    r.flags = flags;

    if (f.has_sen66) {
        r.pm1 = qU16(f.pm1, 10); r.pm25 = qU16(f.pm25, 10);
        r.pm4 = qU16(f.pm4, 10); r.pm10 = qU16(f.pm10, 10);
        r.co2 = f.co2; r.voc = qU16(f.voc, 1); r.nox = qU16(f.nox, 1);
        r.temp = qI16(f.temp, 100); r.rh = qU16(f.rh, 100);
    }
    if (f.has_bh1750) r.lux = qU16(f.lux, 1);
    if (f.has_bme) {
        r.pressure = qU16(f.pressure, 10);
        r.bme_temp = qI16(f.bme_temp, 100); r.bme_rh = qU16(f.bme_rh, 100);
    }
    if (f.has_adxl) {
        r.rumble_rms = qU16(f.rumble_rms, 1000);
        r.rumble_peak = qU16(f.rumble_peak, 1000);
        r.accel_mag = qU16(f.accel_mag, 100);
    }
    if (f.has_co)      r.co_mv = f.co_mv;
    if (f.has_hcho)    r.hcho_mv = f.hcho_mv;
    if (f.has_soil)    r.soil_mv = f.soil_mv;
    if (f.has_battery) r.bat_raw_mv = f.bat_raw_mv;
    if (f.has_mic) {
        r.noise_dba = qI16(f.noise_dba, 10);
        r.noise_spl = qI16(f.noise_spl, 10);
        r.noise_dbfs = qI16(f.noise_dbfs, 10);
        for (int b = 0; b < REC_NBANDS; ++b) r.bands[b] = qU8(f.bands[b]);
    }
    return r;
}

void record_unpack(const Record& r, RecordFields& f) {
    f.seq = r.seq; f.ts = r.ts; f.up_ms = r.up_ms; f.boot = r.boot;
    f.ts_ok      = r.flags & FLAG_TS_OK;
    f.bat_cal    = r.flags & FLAG_BAT_CAL;
    f.noise_clip = r.flags & FLAG_NOISE_CLIP;

    f.has_sen66 = r.flags & PRESENT_SEN66;
    if (f.has_sen66) {
        f.pm1 = r.pm1/10.0f; f.pm25 = r.pm25/10.0f; f.pm4 = r.pm4/10.0f; f.pm10 = r.pm10/10.0f;
        f.co2 = r.co2; f.voc = r.voc; f.nox = r.nox;
        f.temp = r.temp/100.0f; f.rh = r.rh/100.0f;
    }
    f.has_bh1750 = r.flags & PRESENT_BH1750; if (f.has_bh1750) f.lux = r.lux;
    f.has_bme = r.flags & PRESENT_BME;
    if (f.has_bme) { f.pressure = r.pressure/10.0f; f.bme_temp = r.bme_temp/100.0f; f.bme_rh = r.bme_rh/100.0f; }
    f.has_adxl = r.flags & PRESENT_ADXL;
    if (f.has_adxl) { f.rumble_rms = r.rumble_rms/1000.0f; f.rumble_peak = r.rumble_peak/1000.0f; f.accel_mag = r.accel_mag/100.0f; }
    f.has_co = r.flags & PRESENT_CO;       if (f.has_co)      f.co_mv = r.co_mv;
    f.has_hcho = r.flags & PRESENT_HCHO;   if (f.has_hcho)    f.hcho_mv = r.hcho_mv;
    f.has_soil = r.flags & PRESENT_SOIL;   if (f.has_soil)    f.soil_mv = r.soil_mv;
    f.has_battery = r.flags & PRESENT_BATTERY; if (f.has_battery) f.bat_raw_mv = r.bat_raw_mv;
    f.has_mic = r.flags & PRESENT_MIC;
    if (f.has_mic) {
        f.noise_dba = r.noise_dba/10.0f; f.noise_spl = r.noise_spl/10.0f; f.noise_dbfs = r.noise_dbfs/10.0f;
        for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = r.bands[b];
    }
}

// MEMS MOS resistance from the divider — mirrors firmware.cpp gasRs().
static float gasRsLocal(float vout_mv, float rl_ohms) {
    if (vout_mv < 1.0f) return INFINITY;
    return rl_ohms * (GAS_VCC_MV - vout_mv) / vout_mv;
}

void record_to_json(const Record& r, JsonDocument& doc) {
    RecordFields f; record_unpack(r, f);

    doc["ts"]    = f.ts;
    doc["ts_ok"] = f.ts_ok;
    doc["dev"]   = DEVICE_ID;
    doc["up_ms"] = f.up_ms;
    doc["boot"]  = f.boot;

    if (f.has_sen66) {
        doc["pm1"] = f.pm1; doc["pm25"] = f.pm25; doc["pm4"] = f.pm4; doc["pm10"] = f.pm10;
        doc["co2"] = f.co2; doc["voc"] = f.voc; doc["nox"] = f.nox;
        doc["temp"] = f.temp; doc["rh"] = f.rh;
    }
    if (f.has_bh1750) doc["lux"] = f.lux;
    if (f.has_bme) {
        doc["pressure_hpa"] = f.pressure; doc["bme_temp"] = f.bme_temp; doc["bme_rh"] = f.bme_rh;
    }
    if (f.has_adxl) {
        doc["rumble"] = f.rumble_rms; doc["rumble_peak"] = f.rumble_peak; doc["accel_mag"] = f.accel_mag;
    }
    if (f.has_co) {
        doc["co_mv"] = f.co_mv; doc["co_rs"] = gasRsLocal(f.co_mv, GAS_CO_RL_OHMS);
    }
    if (f.has_hcho) {
        doc["hcho_mv"] = f.hcho_mv; doc["hcho_rs"] = gasRsLocal(f.hcho_mv, GAS_HCHO_RL_OHMS);
    }
    if (f.has_soil) {
        float pct = 100.0f * (float)(SOIL_DRY_MV - (int)f.soil_mv) / (float)(SOIL_DRY_MV - SOIL_WET_MV);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        doc["soil_mv"] = f.soil_mv; doc["soil_pct"] = pct;
    }
    if (f.has_battery) {
        doc["bat_raw_mv"] = f.bat_raw_mv; doc["bat_cal"] = f.bat_cal;
        if (f.bat_cal) {
            float v = f.bat_raw_mv / 1000.0f * BAT_DIVIDER_FACTOR;
            float pct = (v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            doc["bat_v"] = v; doc["bat_pct"] = pct;
        }
    }
    if (f.has_mic) {
        doc["noise_dba"] = f.noise_dba; doc["noise_spl"] = f.noise_spl;
        doc["noise_dbfs"] = f.noise_dbfs; doc["noise_clip"] = f.noise_clip;
        JsonArray bands = doc["noise_bands"].to<JsonArray>();
        for (int b = 0; b < REC_NBANDS; ++b) bands.add(f.bands[b]);
    }
}
```

- [ ] **Step 2: Run the record tests to verify they pass**

Run: `cd firmware && pio test -e native -f test_record`
Expected: PASS — all 4 tests green.

- [ ] **Step 3: Commit**

```bash
git add firmware/src/record.cpp
git commit -m "feat(record): pack/unpack + JSON expansion with derived values"
```

---

## Task 5: `record_to_json` produces the legacy field set — test

**Files:**
- Modify: `firmware/test/test_record/test_record.cpp`

- [ ] **Step 1: Add a to_json field-fidelity test**

Add these functions to `firmware/test/test_record/test_record.cpp` (above `main`), and register them in `main`:

```cpp
#include <ArduinoJson.h>

void test_to_json_has_expected_keys_and_derived() {
    RecordFields in = sample();
    Record r = record_pack(in);
    JsonDocument doc;
    record_to_json(r, doc);

    TEST_ASSERT_TRUE(doc["ts"].is<uint32_t>());
    TEST_ASSERT_EQUAL_STRING(DEVICE_ID, doc["dev"]);
    TEST_ASSERT_TRUE(doc["co_rs"].is<float>());          // derived from co_mv
    TEST_ASSERT_TRUE(doc["soil_pct"].is<float>());       // derived from soil_mv
    TEST_ASSERT_TRUE(doc["bat_v"].is<float>());          // derived (bat_cal=true)
    TEST_ASSERT_TRUE(doc["noise_bands"].is<JsonArray>());
    TEST_ASSERT_EQUAL_UINT(REC_NBANDS, doc["noise_bands"].as<JsonArray>().size());

    // co_rs must equal RL*(Vcc-v)/v with v=410mV, RL=GAS_CO_RL_OHMS
    float expect = GAS_CO_RL_OHMS * (GAS_VCC_MV - 410.0f) / 410.0f;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expect, doc["co_rs"].as<float>());
}

void test_to_json_omits_absent_sensor_keys() {
    RecordFields in; in.seq = 1; in.ts = 1719400000;   // no sensors present
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    TEST_ASSERT_FALSE(doc["pm25"].is<float>());
    TEST_ASSERT_FALSE(doc["noise_bands"].is<JsonArray>());
}
```

Register in `main`:

```cpp
    RUN_TEST(test_to_json_has_expected_keys_and_derived);
    RUN_TEST(test_to_json_omits_absent_sensor_keys);
```

- [ ] **Step 2: Run to verify pass**

Run: `cd firmware && pio test -e native -f test_record`
Expected: PASS — 6 tests green.

- [ ] **Step 3: Commit**

```bash
git add firmware/test/test_record/test_record.cpp
git commit -m "test(record): JSON expansion fidelity + derived values"
```

---

## Task 6: `ringlogic` — header

**Files:**
- Create: `firmware/include/ringlogic.h`

- [ ] **Step 1: Write the ringlogic header**

Create `firmware/include/ringlogic.h`:

```cpp
#pragma once
// Pure FIFO ring pointer arithmetic — no IO, fully host-testable. ringstore
// binds this to a LittleFS file. Sequence numbers are monotonic; a record's
// slot is seq % capacity. Drop-oldest on overflow.

#include <stdint.h>

struct RingLogic {
    uint32_t capacity   = 0;
    uint32_t head_seq   = 0;   // next seq to write
    uint32_t tail_seq   = 0;   // oldest seq still stored
    uint32_t synced_seq = 0;   // acked boundary (tail <= synced <= head)
};

void     ring_init(RingLogic& r, uint32_t capacity);

// Reserve the next write slot. Advances head; on overflow advances tail
// (drop-oldest) and synced if it was overtaken. Returns the slot index to write.
uint32_t ring_push_slot(RingLogic& r);

uint32_t ring_count(const RingLogic& r);      // head - tail
uint32_t ring_unsynced(const RingLogic& r);   // head - synced

// Fill up to maxN slot indices + seqs for the unsynced span [synced, head).
// Returns how many were filled.
uint32_t ring_drain_slots(const RingLogic& r, uint32_t maxN,
                          uint32_t* outSlots, uint32_t* outSeqs);

// Advance synced up to (and including) throughSeq, clamped to [synced, head].
void     ring_mark_synced(RingLogic& r, uint32_t throughSeq);
```

- [ ] **Step 2: Commit**

```bash
git add firmware/include/ringlogic.h
git commit -m "feat(ringlogic): FIFO pointer-math header"
```

---

## Task 7: `ringlogic` — failing tests

**Files:**
- Create: `firmware/test/test_ringlogic/test_ringlogic.cpp`

- [ ] **Step 1: Write the tests**

Create `firmware/test/test_ringlogic/test_ringlogic.cpp`:

```cpp
#include <unity.h>
#include "ringlogic.h"

void setUp() {}
void tearDown() {}

void test_push_advances_head_and_slot_wraps() {
    RingLogic r; ring_init(r, 4);
    TEST_ASSERT_EQUAL_UINT32(0, ring_push_slot(r));   // seq0 -> slot0
    TEST_ASSERT_EQUAL_UINT32(1, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(2, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(3, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(0, ring_push_slot(r));   // seq4 -> slot0 (wrap)
    TEST_ASSERT_EQUAL_UINT32(4, ring_count(r));         // still 4 (capacity)
}

void test_overflow_drops_oldest() {
    RingLogic r; ring_init(r, 3);
    for (int i = 0; i < 5; ++i) ring_push_slot(r);    // push seq 0..4
    TEST_ASSERT_EQUAL_UINT32(3, ring_count(r));
    TEST_ASSERT_EQUAL_UINT32(2, r.tail_seq);            // oldest kept is seq2
    TEST_ASSERT_EQUAL_UINT32(5, r.head_seq);
}

void test_drain_and_mark_synced() {
    RingLogic r; ring_init(r, 8);
    for (int i = 0; i < 5; ++i) ring_push_slot(r);    // seq 0..4
    uint32_t slots[8], seqs[8];
    uint32_t n = ring_drain_slots(r, 8, slots, seqs);
    TEST_ASSERT_EQUAL_UINT32(5, n);
    TEST_ASSERT_EQUAL_UINT32(0, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(4, seqs[4]);

    ring_mark_synced(r, 2);                            // acked through seq2 (3 records: 0,1,2)
    TEST_ASSERT_EQUAL_UINT32(3, r.synced_seq);          // synced_seq is exclusive upper bound
    TEST_ASSERT_EQUAL_UINT32(2, ring_unsynced(r));      // seq3,4 remain
    n = ring_drain_slots(r, 8, slots, seqs);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(3, seqs[0]);
}

void test_overflow_past_unsynced_pulls_synced_forward() {
    RingLogic r; ring_init(r, 3);
    for (int i = 0; i < 3; ++i) ring_push_slot(r);    // seq 0,1,2 ; none synced
    ring_push_slot(r);                                 // seq3 overwrites seq0 (unsynced lost)
    TEST_ASSERT_EQUAL_UINT32(1, r.tail_seq);
    TEST_ASSERT_TRUE(r.synced_seq >= r.tail_seq);       // invariant holds
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_push_advances_head_and_slot_wraps);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_drain_and_mark_synced);
    RUN_TEST(test_overflow_past_unsynced_pulls_synced_forward);
    return UNITY_END();
}
```

> Note on `ring_mark_synced` semantics: `synced_seq` is the **exclusive** upper bound of acked records (it equals the next un-acked seq). `ring_mark_synced(r, throughSeq)` sets `synced_seq = throughSeq + 1` (clamped to `head_seq`). The drain in firmware passes the **last** seq it got a 200 for.

- [ ] **Step 2: Run to verify it fails**

Run: `cd firmware && pio test -e native -f test_ringlogic`
Expected: FAIL — ringlogic functions undefined.

- [ ] **Step 3: Commit**

```bash
git add firmware/test/test_ringlogic/test_ringlogic.cpp
git commit -m "test(ringlogic): push/overflow/drain/sync (failing)"
```

---

## Task 8: `ringlogic` — implementation

**Files:**
- Create: `firmware/src/ringlogic.cpp`

- [ ] **Step 1: Implement ringlogic.cpp**

Create `firmware/src/ringlogic.cpp`:

```cpp
#include "ringlogic.h"

void ring_init(RingLogic& r, uint32_t capacity) {
    r.capacity = capacity;
    r.head_seq = r.tail_seq = r.synced_seq = 0;
}

uint32_t ring_push_slot(RingLogic& r) {
    uint32_t slot = r.head_seq % r.capacity;
    r.head_seq++;
    if (r.head_seq - r.tail_seq > r.capacity) {
        r.tail_seq = r.head_seq - r.capacity;          // drop oldest
        if (r.synced_seq < r.tail_seq) r.synced_seq = r.tail_seq;
    }
    return slot;
}

uint32_t ring_count(const RingLogic& r)    { return r.head_seq - r.tail_seq; }
uint32_t ring_unsynced(const RingLogic& r) { return r.head_seq - r.synced_seq; }

uint32_t ring_drain_slots(const RingLogic& r, uint32_t maxN,
                          uint32_t* outSlots, uint32_t* outSeqs) {
    uint32_t n = 0;
    for (uint32_t seq = r.synced_seq; seq < r.head_seq && n < maxN; ++seq, ++n) {
        outSlots[n] = seq % r.capacity;
        outSeqs[n]  = seq;
    }
    return n;
}

void ring_mark_synced(RingLogic& r, uint32_t throughSeq) {
    uint32_t next = throughSeq + 1;
    if (next > r.head_seq) next = r.head_seq;
    if (next > r.synced_seq) r.synced_seq = next;
}
```

- [ ] **Step 2: Run to verify pass**

Run: `cd firmware && pio test -e native -f test_ringlogic`
Expected: PASS — 4 tests green.

- [ ] **Step 3: Run the full native suite**

Run: `cd firmware && pio test -e native`
Expected: PASS — both test_record and test_ringlogic green.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/ringlogic.cpp
git commit -m "feat(ringlogic): FIFO pointer-math implementation"
```

---

## Task 9: config.h — ring + duty-cycle + mode constants

**Files:**
- Modify: `firmware/include/config.h`

- [ ] **Step 1: Add the new constants and retire the queue ones**

In `firmware/include/config.h`, replace the `--- Durable queue on flash (LittleFS) ---` block (the `QUEUE_*` + `SYNC_BATCH_*` lines, ~155–162) with:

```cpp
// --- Binary FIFO ring buffer on flash (LittleFS) ---
// Fixed-size packed records (see record.h, RECORD_SIZE=71). Drop-oldest when
// full. Pointers persisted in a double-buffered, CRC'd meta file.
#define RING_PATH            "/ring.bin"
#define RING_META_PATH       "/ring.meta"
#define RING_CAPACITY        18000U          // 18000*71B ~= 1.28 MB ~= ~12.5 days @ 60s
#define SYNC_BATCH_MAX       20              // records POSTed per HTTP request
#define SYNC_BATCH_MAX_BYTES 4096            // ...or this many bytes, whichever first

// Legacy NDJSON queue paths — removed on boot if present (no migration needed).
#define LEGACY_QUEUE_PATH    "/queue.ndjson"
#define LEGACY_CURSOR_PATH   "/cursor.txt"

// --- Duty-cycled sync + modes ---
// NORMAL: WiFi off between sessions; attempt to offload every SYNC_ATTEMPT_INTERVAL_MS,
//         or early once SYNC_THRESHOLD_RECORDS are buffered.
// TESTING: WiFi stays on, every record pushed live. Entered/left via dashboard command.
// BOOT_WINDOW: after every power-on WiFi is up for this long so a dashboard
//              "enter testing" press lands immediately.
#define SYNC_ATTEMPT_INTERVAL_MS  (15UL * 60UL * 1000UL)   // 15 min
#define SYNC_THRESHOLD_RECORDS    120                       // early-trigger for bursts
#define BOOT_WINDOW_MS            (5UL * 60UL * 1000UL)      // 5 min
```

- [ ] **Step 2: Verify the firmware still references no removed symbol yet**

Run: `cd firmware && grep -n "QUEUE_PATH\|QUEUE_CURSOR_PATH\|QUEUE_COMPACT_BYTES" src/firmware.cpp`
Expected: matches in firmware.cpp (still the old code — fixed in Task 12). This is just a note of what Task 12 must replace.

- [ ] **Step 3: Commit**

```bash
git add firmware/include/config.h
git commit -m "config: ring buffer, duty-cycle interval, mode constants"
```

---

## Task 10: `ringstore` — header

**Files:**
- Create: `firmware/include/ringstore.h`

- [ ] **Step 1: Write the ringstore header**

Create `firmware/include/ringstore.h`:

```cpp
#pragma once
// LittleFS-backed binary FIFO ring. Binds ringlogic (pointer math) to a
// preallocated /ring.bin of RING_CAPACITY fixed slots, with pointers persisted
// in a double-buffered CRC'd /ring.meta. Worst-case loss on a power cut is the
// single in-flight record (data is written before metadata).

#include "record.h"
#include <stdint.h>

// Mount LittleFS-resident state: load+validate meta (or init), preallocate
// /ring.bin if absent, and delete any legacy NDJSON queue files. Call once in
// setup() AFTER LittleFS.begin(). Returns false on unrecoverable FS error.
bool ringstore_begin();

// Append one record (drop-oldest if full). Returns false on write error.
bool ringstore_push(const Record& rec);

// Read up to maxN un-synced records (oldest first) into out[]; also returns the
// seq of the last one in *lastSeq. Returns the count read.
uint32_t ringstore_drain(Record* out, uint32_t maxN, uint32_t* lastSeq);

// Mark everything through lastSeq as synced and persist the new pointer.
void ringstore_mark_synced(uint32_t lastSeq);

uint32_t ringstore_count(void);      // total stored
uint32_t ringstore_unsynced(void);   // not yet acked
```

- [ ] **Step 2: Commit**

```bash
git add firmware/include/ringstore.h
git commit -m "feat(ringstore): LittleFS ring API header"
```

---

## Task 11: `ringstore` — implementation

**Files:**
- Create: `firmware/src/ringstore.cpp`

- [ ] **Step 1: Implement ringstore.cpp**

Create `firmware/src/ringstore.cpp`:

```cpp
#include "ringstore.h"
#include "ringlogic.h"
#include "config.h"
#include <LittleFS.h>
#include <Arduino.h>

static RingLogic s_logic;

// On-flash metadata, written to two alternating slots in /ring.meta.
struct Meta {
    uint32_t magic;        // 'A''R''N''G'
    uint8_t  schema;       // RECORD_SCHEMA_VERSION
    uint8_t  rec_size;     // RECORD_SIZE
    uint16_t _pad;
    uint32_t capacity;
    uint32_t head_seq;
    uint32_t tail_seq;
    uint32_t synced_seq;
    uint32_t write_counter;
    uint32_t crc;          // crc32 over all preceding bytes
};
static constexpr uint32_t META_MAGIC = 0x41524E47;  // "ARNG"
static uint32_t s_writeCounter = 0;

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320 & (-(int)(c & 1)));
    }
    return ~c;
}

static bool metaValid(const Meta& m) {
    if (m.magic != META_MAGIC) return false;
    uint32_t want = crc32((const uint8_t*)&m, sizeof(Meta) - sizeof(uint32_t));
    return want == m.crc && m.capacity == RING_CAPACITY &&
           m.rec_size == RECORD_SIZE && m.schema == RECORD_SCHEMA_VERSION;
}

static void persistMeta() {
    Meta m{};
    m.magic = META_MAGIC; m.schema = RECORD_SCHEMA_VERSION; m.rec_size = RECORD_SIZE;
    m.capacity = s_logic.capacity;
    m.head_seq = s_logic.head_seq; m.tail_seq = s_logic.tail_seq; m.synced_seq = s_logic.synced_seq;
    m.write_counter = ++s_writeCounter;
    m.crc = crc32((const uint8_t*)&m, sizeof(Meta) - sizeof(uint32_t));

    // alternate between slot 0 and 1 so a torn write never destroys both copies
    size_t off = (s_writeCounter & 1) ? sizeof(Meta) : 0;
    File f = LittleFS.open(RING_META_PATH, "r+");
    if (!f) f = LittleFS.open(RING_META_PATH, "w+");
    if (!f) { Serial.println("[ring] meta open failed"); return; }
    f.seek(off);
    f.write((const uint8_t*)&m, sizeof(Meta));
    f.close();
}

static bool loadMeta() {
    File f = LittleFS.open(RING_META_PATH, "r");
    if (!f) return false;
    Meta a{}, b{};
    f.read((uint8_t*)&a, sizeof(Meta));
    f.read((uint8_t*)&b, sizeof(Meta));
    f.close();
    bool va = metaValid(a), vb = metaValid(b);
    const Meta* pick = nullptr;
    if (va && vb) pick = (a.write_counter >= b.write_counter) ? &a : &b;
    else if (va)  pick = &a;
    else if (vb)  pick = &b;
    if (!pick) return false;
    s_logic.capacity = pick->capacity;
    s_logic.head_seq = pick->head_seq; s_logic.tail_seq = pick->tail_seq; s_logic.synced_seq = pick->synced_seq;
    s_writeCounter = pick->write_counter;
    return true;
}

static bool preallocate() {
    File f = LittleFS.open(RING_PATH, "r");
    size_t want = (size_t)RING_CAPACITY * RECORD_SIZE;
    if (f && f.size() == want) { f.close(); return true; }
    if (f) f.close();
    f = LittleFS.open(RING_PATH, "w");
    if (!f) return false;
    uint8_t zero[256] = {0};
    size_t left = want;
    while (left) { size_t n = left < sizeof(zero) ? left : sizeof(zero); f.write(zero, n); left -= n; }
    f.close();
    Serial.printf("[ring] preallocated %u bytes\n", (unsigned)want);
    return true;
}

bool ringstore_begin() {
    if (LittleFS.exists(LEGACY_QUEUE_PATH))  LittleFS.remove(LEGACY_QUEUE_PATH);
    if (LittleFS.exists(LEGACY_CURSOR_PATH)) LittleFS.remove(LEGACY_CURSOR_PATH);

    ring_init(s_logic, RING_CAPACITY);
    if (!preallocate()) { Serial.println("[ring] preallocate failed"); return false; }
    if (!loadMeta()) {
        Serial.println("[ring] no valid meta — starting fresh");
        persistMeta();
    } else {
        Serial.printf("[ring] resumed head=%u tail=%u synced=%u\n",
                      s_logic.head_seq, s_logic.tail_seq, s_logic.synced_seq);
    }
    return true;
}

bool ringstore_push(const Record& rec) {
    uint32_t slot = ring_push_slot(s_logic);
    File f = LittleFS.open(RING_PATH, "r+");
    if (!f) { Serial.println("[ring] push open failed"); return false; }
    f.seek((size_t)slot * RECORD_SIZE);
    Record r = rec; r.seq = s_logic.head_seq - 1;     // stamp the assigned seq
    size_t w = f.write((const uint8_t*)&r, RECORD_SIZE);
    f.close();
    if (w != RECORD_SIZE) { Serial.println("[ring] short write"); return false; }
    persistMeta();                                     // data first, then meta
    return true;
}

uint32_t ringstore_drain(Record* out, uint32_t maxN, uint32_t* lastSeq) {
    uint32_t slots[SYNC_BATCH_MAX], seqs[SYNC_BATCH_MAX];
    if (maxN > SYNC_BATCH_MAX) maxN = SYNC_BATCH_MAX;
    uint32_t n = ring_drain_slots(s_logic, maxN, slots, seqs);
    if (n == 0) return 0;
    File f = LittleFS.open(RING_PATH, "r");
    if (!f) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        f.seek((size_t)slots[i] * RECORD_SIZE);
        f.read((uint8_t*)&out[i], RECORD_SIZE);
    }
    f.close();
    if (lastSeq) *lastSeq = seqs[n - 1];
    return n;
}

void ringstore_mark_synced(uint32_t lastSeq) {
    ring_mark_synced(s_logic, lastSeq);
    persistMeta();
}

uint32_t ringstore_count(void)    { return ring_count(s_logic); }
uint32_t ringstore_unsynced(void) { return ring_unsynced(s_logic); }
```

- [ ] **Step 2: Add ringstore + record + ringlogic to the phase1 build**

In `firmware/platformio.ini`, change the phase1 `build_src_filter`:

```ini
build_src_filter = +<firmware.cpp> +<mic.cpp> +<accel.cpp> +<record.cpp> +<ringlogic.cpp> +<ringstore.cpp>
```

- [ ] **Step 3: Compile-check the firmware env (will still fail on firmware.cpp until Task 12)**

Run: `cd firmware && pio run -e esp32_phase1 2>&1 | tail -20`
Expected: ringstore.cpp/record.cpp/ringlogic.cpp compile; firmware.cpp may still reference old queue symbols — that's fixed next task. Note any ringstore compile errors and fix them here.

- [ ] **Step 4: Commit**

```bash
git add firmware/src/ringstore.cpp firmware/platformio.ini
git commit -m "feat(ringstore): LittleFS ring with double-buffered CRC metadata"
```

---

## Task 12: firmware.cpp — swap queue for ring, add modes + duty-cycled sync

**Files:**
- Modify: `firmware/src/firmware.cpp`

- [ ] **Step 1: Replace includes and add module headers**

In `firmware/src/firmware.cpp`, after the existing `#include "accel.h"` (line ~37) add:

```cpp
#include "record.h"
#include "ringstore.h"
```

- [ ] **Step 2: Remove the NDJSON queue functions**

Delete these functions entirely from `firmware.cpp`: `cursorRead`, `cursorWrite`, `queueAppendLine`, `queueCompactIfDone` (lines ~188–219), and the old `syncQueue` (lines ~321–377). They are replaced below.

- [ ] **Step 3: Replace `buildRecord()` with `buildFields()` that fills a RecordFields**

Replace the whole `buildRecord()` function (lines ~225–315) with:

```cpp
// Build one RecordFields from a fresh read of every present sensor.
static void buildFields(RecordFields& f) {
    f.ts    = (uint32_t)time(nullptr);
    f.ts_ok = timeIsValid();
    f.up_ms = millis();
    f.boot  = (uint16_t)(g_bootId & 0xFFFF);

    if (present.sen66) {
        float pm1, pm25, pm4, pm10, t, rh, voc, nox; uint16_t co2;
        if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, rh, t, voc, nox, co2) == 0) {
            f.has_sen66 = true;
            f.pm1 = pm1; f.pm25 = pm25; f.pm4 = pm4; f.pm10 = pm10;
            f.co2 = co2; f.voc = voc; f.nox = nox; f.temp = t; f.rh = rh;
        }
    }
    if (present.bh1750 && bh1750.measurementReady(true)) {
        f.has_bh1750 = true; f.lux = bh1750.readLightLevel();
    }
    if (present.bme) {
        f.has_bme = true;
        f.pressure = bme.readPressure() / 100.0f;
        f.bme_temp = bme.readTemperature();
        f.bme_rh   = bme.readHumidity();
    }
    if (present.adxl) {
        AccelResult a;
        if (accel_capture(adxl, a)) {
            f.has_adxl = true;
            f.rumble_rms = a.rumble_rms; f.rumble_peak = a.rumble_peak; f.accel_mag = a.mag_mean;
        }
    }
    if (present.co)   { f.has_co = true;   f.co_mv   = (uint16_t)readAdcMv(PIN_GAS_CO_ADC); }
    if (present.hcho) { f.has_hcho = true; f.hcho_mv = (uint16_t)readAdcMv(PIN_GAS_HCHO_ADC); }
    if (present.soil) { f.has_soil = true; f.soil_mv = (uint16_t)readAdcMv(PIN_SOIL_ADC); }
    if (present.battery) {
        f.has_battery = true;
        f.bat_raw_mv = (uint16_t)readAdcMv(PIN_BATTERY_ADC);
        f.bat_cal = (bool)BAT_CALIBRATED;
    }
    if (present.mic) {
        MicResult m;
        if (mic_capture(m)) {
            f.has_mic = true;
            f.noise_dba = m.laeq_est; f.noise_spl = m.spl_est; f.noise_dbfs = m.rms_dbfs;
            f.noise_clip = m.clipping;
            for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = m.band_dba[b];
        }
    }
}
```

- [ ] **Step 4: Add the mode state machine + sync session**

Add near the top of `firmware.cpp` (after the `g_bootId` definition, ~line 61):

```cpp
enum Mode { MODE_NORMAL, MODE_TESTING };
static Mode     g_mode           = MODE_NORMAL;
static uint32_t g_bootStartMs    = 0;
static uint32_t g_lastSyncAttempt = 0;

static bool inBootWindow() { return (millis() - g_bootStartMs) < BOOT_WINDOW_MS; }
static bool wifiShouldStayOn() { return g_mode == MODE_TESTING || inBootWindow(); }
```

Then add the new sync code (where the old `syncQueue` was):

```cpp
// Apply a {"command":{"set_mode": "..."}} returned by the server.
static void applyServerCommand(const JsonDocument& doc) {
    const char* sm = doc["command"]["set_mode"] | (const char*)nullptr;
    if (!sm) return;
    if (!strcmp(sm, "testing") && g_mode != MODE_TESTING) {
        g_mode = MODE_TESTING; Serial.println("[mode] -> TESTING (server)");
    } else if (!strcmp(sm, "normal") && g_mode != MODE_NORMAL) {
        g_mode = MODE_NORMAL;  Serial.println("[mode] -> NORMAL (server)");
    }
}

// POST one batch of records as an envelope. Returns the HTTP code; on success
// advances the synced pointer and applies any server command.
static int postBatch(const Record* recs, uint32_t n, uint32_t lastSeq) {
    String   host = s_serverHost.length() ? s_serverHost : String(SYNC_HOST);
    uint16_t port = s_serverHost.length() ? s_serverPort : (uint16_t)SYNC_PORT;

    JsonDocument doc;
    doc["dev"]      = DEVICE_ID;
    doc["boot"]     = g_bootId;
    doc["fw"]       = "phase1";
    doc["mode"]     = (g_mode == MODE_TESTING) ? "testing" : "normal";
    doc["buffered"] = ringstore_unsynced();
    JsonArray arr = doc["records"].to<JsonArray>();
    for (uint32_t i = 0; i < n; ++i) {
        JsonObject o = arr.add<JsonObject>();
        record_to_json(recs[i], o);
    }
    String body; serializeJson(doc, body);

    HTTPClient http;
    String url = String("http://") + host + ":" + String(port) + SYNC_PATH;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(8000);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();

    if (code == 200 || code == 201 || code == 204) {
        JsonDocument rdoc;
        if (!deserializeJson(rdoc, resp)) { adoptServerTime(resp); applyServerCommand(rdoc); }
        ringstore_mark_synced(lastSeq);
        Serial.printf("[sync] %u acked (unsynced now %u)\n", n, ringstore_unsynced());
        ledPulse();
        return code;
    }
    Serial.printf("[sync] POST failed code=%d\n", code);
    s_serverHost = "";     // force re-discovery next time
    return code;
}

// Drain the ring to the server in batches until empty or a POST fails.
static void drainRing() {
    if (WiFi.status() != WL_CONNECTED) return;
    Record batch[SYNC_BATCH_MAX];
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t lastSeq = 0;
        uint32_t n = ringstore_drain(batch, SYNC_BATCH_MAX, &lastSeq);
        if (n == 0) break;
        if (postBatch(batch, n, lastSeq) >= 400 || WiFi.status() != WL_CONNECTED) break;
        if (ringstore_unsynced() == 0) break;
    }
}

// Bring WiFi up, sync time/discover, drain, and (in NORMAL, outside the boot
// window) drop WiFi to save power.
static void syncSession() {
    g_lastSyncAttempt = millis();
    if (wifiConnect()) {
        syncTimeIfNeeded();
        if (s_serverHost.length() == 0) discoverServer();
        drainRing();
    }
    if (g_mode == MODE_NORMAL && !inBootWindow()) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
    }
}
```

- [ ] **Step 5: Rewrite setup()'s tail and loop()**

In `setup()`, replace the first-WiFi block + final prints (lines ~463–470) with:

```cpp
    g_bootStartMs = millis();
    if (!ringstore_begin()) Serial.println("[ring] begin failed");

    // Boot window: WiFi on so the dashboard can flip us into testing mode.
    if (wifiConnect()) { syncTimeIfNeeded(); discoverServer(); }

    ledBlink(3);
    digitalWrite(PIN_EXT_LED, LOW);
    Serial.println("[boot] ready — sampling every "
                   + String(SAMPLE_BASELINE_MS / 1000) + " s");
```

Replace the entire `loop()` (lines ~472–505) with:

```cpp
void loop() {
    static uint32_t tSample = 0;
    const uint32_t now = millis();

    // --- fixed-cadence baseline sample ---
    if (tSample == 0 || now - tSample >= SAMPLE_BASELINE_MS) {
        tSample = now;

        if (wifiShouldStayOn() && WiFi.status() != WL_CONNECTED) wifiConnect();
        if (WiFi.status() == WL_CONNECTED) { syncTimeIfNeeded();
            if (s_serverHost.length() == 0) discoverServer(); }

        RecordFields f; buildFields(f);
        Record rec = record_pack(f);
        ringstore_push(rec);
        Serial.printf("[rec] seq=%u ts=%u buffered=%u mode=%s\n",
                      rec.seq, rec.ts, ringstore_unsynced(),
                      g_mode == MODE_TESTING ? "testing" : "normal");

        // TESTING: push live right after each sample.
        if (g_mode == MODE_TESTING && WiFi.status() == WL_CONNECTED) drainRing();
    }

    // --- decide whether to run a sync session ---
    if (g_mode == MODE_TESTING) {
        // stay connected; live drain already handled at sample time
    } else if (inBootWindow()) {
        // keep WiFi up and poll the server so an "enter testing" command lands fast
        if (now - g_lastSyncAttempt >= 5000) syncSession();
    } else {
        bool dueByTime  = (now - g_lastSyncAttempt) >= SYNC_ATTEMPT_INTERVAL_MS;
        bool dueByCount = ringstore_unsynced() >= SYNC_THRESHOLD_RECORDS;
        if (g_lastSyncAttempt == 0 || dueByTime || dueByCount) syncSession();
    }

    delay(50);
}
```

- [ ] **Step 6: Build the firmware**

Run: `cd firmware && pio run -e esp32_phase1 2>&1 | tail -25`
Expected: SUCCESS — clean link. Fix any compile errors (typically a missed reference to a deleted queue symbol).

- [ ] **Step 7: Commit**

```bash
git add firmware/src/firmware.cpp
git commit -m "feat(firmware): binary ring storage, duty-cycled sync, testing mode"
```

---

## Task 13: PC server — envelope ingest + per-device state + command channel

**Files:**
- Modify: `server/server.py`

- [ ] **Step 1: Add device-state structures and command endpoints**

In `server/server.py`, after the `hub = Hub()` line (~162) add:

```python
# --------------------------------------------------------------------------- #
# Per-device live state + a one-shot command the dashboard can queue for a
# device. The device reports its mode/buffered count on every /ingest; we return
# any pending command in the reply and clear it once the device echoes the mode.
# --------------------------------------------------------------------------- #
DEVICE_STATE: dict[str, dict] = {}     # dev -> {mode,last_seen,buffered,boot,fw}
PENDING_CMD: dict[str, str] = {}       # dev -> "testing" | "normal"


def _update_device(dev: str, mode: str, buffered, boot, fw) -> None:
    DEVICE_STATE[dev] = {
        "dev": dev, "mode": mode, "buffered": buffered,
        "boot": boot, "fw": fw, "last_seen": int(time.time()),
    }
    # Clear a pending command once the device reports the target mode.
    if PENDING_CMD.get(dev) == mode:
        PENDING_CMD.pop(dev, None)
```

- [ ] **Step 2: Rewrite `/ingest` to accept the envelope and return a command**

Replace the body of the `ingest` function (the part after `body = await request.json()`) so it handles `{records:[...], dev, mode, ...}` as well as the legacy list/dict:

```python
@app.post("/ingest")
async def ingest(request: Request):
    """Accept an envelope {dev,mode,buffered,boot,fw,records:[...]} or, for
    backward compatibility, a bare record dict or a list of records."""
    body = await request.json()

    if isinstance(body, dict) and "records" in body:
        records = body["records"]
        dev = body.get("dev")
        mode = body.get("mode", "normal")
        if dev:
            _update_device(dev, mode, body.get("buffered"), body.get("boot"), body.get("fw"))
    else:
        records = body if isinstance(body, list) else [body]

    stored = []
    corrected = 0
    async with _db_lock:
        boots: set[tuple] = set()
        for rec in records:
            if not isinstance(rec, dict):
                continue
            if _dedupe_insert(rec):
                stored.append(rec)
            if rec.get("ts_ok", True) and rec.get("boot") is not None:
                boots.add((rec.get("dev") or rec.get("device"), rec.get("boot")))
        for device, boot in boots:
            corrected += _backfill_times(device, boot)
        _conn.commit()
    if corrected:
        print(f"[time] back-filled {corrected} record(s) with a corrected timestamp")

    for rec in stored:
        await hub.broadcast({"type": "reading", "data": rec})

    # Tell dashboards about the device's current state.
    dev = body.get("dev") if isinstance(body, dict) else None
    if dev and dev in DEVICE_STATE:
        await hub.broadcast({"type": "device", "data": DEVICE_STATE[dev]})

    resp = {"ok": True, "received": len(records), "stored": len(stored),
            "server_time": int(time.time())}
    if dev and dev in PENDING_CMD:
        resp["command"] = {"set_mode": PENDING_CMD[dev]}
    return resp
```

- [ ] **Step 3: Add the device list + command endpoints**

Add after the `/api/stats` route (~338):

```python
# ---- device mode (testing) control --------------------------------------- #
@app.get("/api/devices")
async def get_devices():
    """Current per-device state, with any pending command attached."""
    out = []
    for dev, st in DEVICE_STATE.items():
        d = dict(st)
        d["pending"] = PENDING_CMD.get(dev)
        out.append(d)
    return JSONResponse(out)


@app.post("/api/device/{dev}/mode")
async def set_device_mode(dev: str, request: Request):
    """Queue a mode switch for a device. Applied next time it contacts /ingest
    (immediately while it's online in the boot window or in testing mode)."""
    body = await request.json()
    mode = body.get("mode")
    if mode not in ("testing", "normal"):
        return JSONResponse({"error": "mode must be 'testing' or 'normal'"}, status_code=400)
    PENDING_CMD[dev] = mode
    if dev in DEVICE_STATE:
        DEVICE_STATE[dev]["pending"] = mode
        await hub.broadcast({"type": "device", "data": {**DEVICE_STATE[dev], "pending": mode}})
    return {"ok": True, "dev": dev, "pending": mode}
```

- [ ] **Step 4: Commit**

```bash
git add server/server.py
git commit -m "feat(server): envelope ingest, per-device state, testing-mode command channel"
```

---

## Task 14: PC server — pytest for envelope + command flow

**Files:**
- Create: `server/test_server.py`

- [ ] **Step 1: Write the failing/clean test**

Create `server/test_server.py`:

```python
"""Tests for the envelope ingest + testing-mode command channel.

Run:  cd server && AIRMON_DB=:memory: pytest test_server.py -v
(requires: pip install pytest httpx)
"""
import importlib
import os

os.environ["AIRMON_DB"] = ":memory:"     # don't touch the real db

from fastapi.testclient import TestClient
import server as srv

client = TestClient(srv.app)


def _envelope(mode="normal", buffered=3):
    return {
        "dev": "air-monitor-01", "boot": 999, "fw": "phase1",
        "mode": mode, "buffered": buffered,
        "records": [
            {"ts": 1719400000, "ts_ok": True, "dev": "air-monitor-01",
             "up_ms": 1000, "boot": 999, "pm25": 4.2, "co2": 800},
        ],
    }


def test_envelope_ingest_stores_record_and_device_state():
    with TestClient(srv.app) as c:
        r = c.post("/ingest", json=_envelope())
        assert r.status_code == 200
        assert r.json()["stored"] == 1
        devs = c.get("/api/devices").json()
        assert any(d["dev"] == "air-monitor-01" and d["mode"] == "normal" for d in devs)


def test_legacy_list_still_accepted():
    with TestClient(srv.app) as c:
        r = c.post("/ingest", json=[{"ts": 1719400100, "dev": "old", "up_ms": 1, "boot": 1}])
        assert r.status_code == 200
        assert r.json()["received"] == 1


def test_pending_command_returned_then_cleared():
    with TestClient(srv.app) as c:
        c.post("/ingest", json=_envelope(mode="normal"))         # device known
        c.post("/api/device/air-monitor-01/mode", json={"mode": "testing"})
        # next contact (still normal) must receive the command
        r = c.post("/ingest", json=_envelope(mode="normal"))
        assert r.json().get("command", {}).get("set_mode") == "testing"
        # once the device echoes testing, the command clears
        c.post("/ingest", json=_envelope(mode="testing"))
        r = c.post("/ingest", json=_envelope(mode="testing"))
        assert "command" not in r.json()


def test_invalid_mode_rejected():
    with TestClient(srv.app) as c:
        c.post("/ingest", json=_envelope())
        r = c.post("/api/device/air-monitor-01/mode", json={"mode": "bogus"})
        assert r.status_code == 400
```

- [ ] **Step 2: Run the tests**

Run: `cd server && pip install -q pytest httpx && pytest test_server.py -v`
Expected: PASS — 4 tests green. (If `server` import pulls zeroconf and it's missing, the lifespan prints a warning but still runs.)

- [ ] **Step 3: Commit**

```bash
git add server/test_server.py
git commit -m "test(server): envelope ingest + testing-mode command flow"
```

---

## Task 15: Dashboard — device mode badge + Enter/Exit testing buttons

**Files:**
- Modify: `server/static/index.html`

- [ ] **Step 1: Add a device badge in the header**

In `server/static/index.html`, in `<header>` (after the live pill, ~line 95) add:

```html
  <span class="pill" id="devmode" title="Sensor device mode"></span>
```

Add to the `<style>` block (near the `.dot` rules, ~line 24):

```css
  .badge { font-size:11px; font-weight:700; padding:3px 8px; border-radius:8px; white-space:nowrap; }
  .badge.normal  { background:#1f3a5f; color:#bcd7ff; }
  .badge.testing { background:#1f7a44; color:#eafff1; }
  .badge.offline { background:#3a1f23; color:#ffb3bd; }
  .badge.pending { outline:1px dashed #ffce6b; }
```

- [ ] **Step 2: Add a Device section with the toggle button in the controls drawer**

In the controls drawer, after the "Home state" `</section>` (~line 138) add:

```html
    <section>
      <h2>Sensor device</h2>
      <div class="stat" id="devstat">no device seen yet</div>
      <div class="row" style="margin-top:10px">
        <button id="testBtn" class="sec">Enter testing mode</button>
      </div>
      <div class="stat" id="devpending"></div>
    </section>
```

- [ ] **Step 3: Add the device-state JS**

Before the final `(async function init(){ ... })();` block (~line 455) add:

```javascript
// ---- sensor-device mode badge + testing toggle ----------------------------
let deviceState = null;   // {dev,mode,last_seen,buffered,pending}
const OFFLINE_AFTER = 35 * 60;   // > 2x the 15-min sync interval

function renderDevice() {
  const el = $('#devmode'), st = $('#devstat'), pend = $('#devpending'), btn = $('#testBtn');
  if (!deviceState) { el.textContent = ''; return; }
  const age = Math.floor(Date.now()/1000) - (deviceState.last_seen || 0);
  const offline = age > OFFLINE_AFTER;
  const mode = offline ? 'offline' : (deviceState.mode || 'normal');
  el.className = 'pill';
  el.innerHTML = `<span class="badge ${mode}${deviceState.pending ? ' pending' : ''}">${mode.toUpperCase()}</span>`;
  st.textContent = `${deviceState.dev} · ${deviceState.buffered ?? '?'} buffered · seen ${age}s ago`;
  const wantTesting = (deviceState.mode !== 'testing');
  btn.textContent = wantTesting ? 'Enter testing mode' : 'Stop testing mode';
  pend.textContent = deviceState.pending
    ? `pending: switch to ${deviceState.pending} (applies at next sync)` : '';
}

async function loadDevices() {
  const devs = await (await fetch('/api/devices')).json();
  if (devs.length) { deviceState = devs[0]; renderDevice(); }
}

$('#testBtn').onclick = async () => {
  if (!deviceState) return;
  const target = (deviceState.mode === 'testing') ? 'normal' : 'testing';
  await fetch(`/api/device/${encodeURIComponent(deviceState.dev)}/mode`,
    { method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({ mode: target }) });
  deviceState.pending = target; renderDevice();
};
```

- [ ] **Step 4: Handle `device` WS messages and poll on init**

In `connectWS()`'s `ws.onmessage` (~line 313), add a branch alongside the `reading`/`event` ones:

```javascript
    if (msg.type === 'device')  { deviceState = msg.data; renderDevice(); }
```

In the `init` IIFE (~line 455), add `await loadDevices();` after `await query();`, and add `renderDevice` to the periodic refresh:

```javascript
  await loadDevices();
  setInterval(loadDevices, 30000);   // re-poll device state (also drives offline detection)
```

- [ ] **Step 5: Manual smoke test**

Run: `cd server && python server.py` then in another shell:

```bash
curl -s -XPOST localhost:8000/ingest -H 'Content-Type: application/json' \
  -d '{"dev":"air-monitor-01","boot":1,"fw":"phase1","mode":"normal","buffered":5,"records":[]}' | python -m json.tool
curl -s localhost:8000/api/devices | python -m json.tool
curl -s -XPOST localhost:8000/api/device/air-monitor-01/mode -H 'Content-Type: application/json' -d '{"mode":"testing"}'
```

Expected: `/api/devices` shows the device; open `http://localhost:8000/` and confirm a `NORMAL` badge appears, the "Enter testing mode" button queues a pending switch, and the badge shows the dashed pending outline.

- [ ] **Step 6: Commit**

```bash
git add server/static/index.html
git commit -m "feat(dashboard): device mode badge + enter/exit testing controls"
```

---

## Task 16: On-device integration verification

**Files:** (no code; verification + notes)

- [ ] **Step 1: Flash and watch the boot window**

Run: `cd firmware && pio run -e esp32_phase1 -t upload && pio device monitor`
Expected: `[ring] preallocated ...` on first boot (or `[ring] resumed ...` after), `[rec] seq=... buffered=... mode=normal` every 60 s, and during the first 5 min the device contacts the server every ~5 s.

- [ ] **Step 2: Verify testing mode round-trip**

While the device is in the boot window, click "Enter testing mode" on the dashboard. Expected: monitor prints `[mode] -> TESTING (server)`; readings now POST within seconds of each sample; badge shows `TESTING`. Click "Stop testing mode" → `[mode] -> NORMAL (server)`, WiFi drops after the session.

- [ ] **Step 3: Verify drop-oldest + duty cycle (notes)**

Confirm via the monitor that, with the server unreachable, `buffered` climbs and the device retries on the ~15-min cadence (not every loop), and that after a reconnect the backlog drains oldest-first. Full ring overflow (~12 days) is validated by the `ringlogic` unit tests (`test_overflow_drops_oldest`), not on hardware.

- [ ] **Step 4: Update DESIGN.md TODO**

In `DESIGN.md`, change the first TODO line (line 13) from the fifo note to a done marker, e.g.:

```markdown
- ~~fifo in firmware~~ → DONE: binary FIFO ring (~12 days), duty-cycled 15-min sync, dashboard testing mode (see docs/superpowers/specs/2026-06-26-firmware-fifo-ring-buffer-design.md)
```

- [ ] **Step 5: Commit**

```bash
git add DESIGN.md
git commit -m "docs: mark firmware FIFO ring buffer done"
```

---

## Self-Review notes

- **Spec coverage:** binary record (Tasks 2–5), FIFO ring + drop-oldest (Tasks 6–8, 11), capacity/partition unchanged (Task 9 `RING_CAPACITY`), duty-cycled 15-min sync (Tasks 9, 12), testing mode entry/exit via dashboard command + boot window (Tasks 12–13, 15), mode badge on dashboard (Task 15), wire-format preserved via `record_to_json` (Task 4) and legacy-list acceptance (Tasks 13–14). All spec sections map to a task.
- **Type consistency:** `RECORD_SIZE=71` / `REC_NBANDS=9` used consistently in record.h, ringstore.cpp, tests. `ringstore_drain(out, maxN, lastSeq)` / `ringstore_mark_synced(lastSeq)` signatures match their call sites in firmware.cpp. `ring_mark_synced` exclusive-`synced_seq` semantics documented in Task 7 and relied on by `postBatch`→`ringstore_mark_synced(lastSeq)`.
- **No placeholders:** every code step contains full source. The only "notes" step (16.3) is hardware observation, with the overflow guarantee covered by a real unit test.
