# Air Monitor System Design Document

**Project Goal:** A battery-powered multi-sensor environmental monitoring station with two operational modes:
1. **Home Air Quality Monitor** — track indoor air quality, ventilation patterns, occupancy, and device impact
2. **Aircraft Pollution Tracker** — correlate aircraft overhead with local air quality/noise/light measurements and public flight data

**Status:** Initial design phase. This document captures requirements, assumptions, and parameters for implementation. Update as clarifications emerge.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    SYSTEM OVERVIEW                              │
└─────────────────────────────────────────────────────────────────┘

  [ESP32 Sensor Station]                   [LattePanda Alpha PC]
  ├─ 10 sensors (wired)                    ├─ Data collection & storage
  ├─ On-device adaptive sampling           ├─ Dashboard (local graphs)
  ├─ Ring buffer + SD/flash                ├─ Data sync from ESP32
  ├─ Battery-powered                       ├─ Weather API fetch
  ├─ WiFi/BLE intermittent                 ├─ Aircraft ADS-B decode (RTL-SDR)
  └─ ~240 MHz, 520 KB SRAM, 4 MB flash     └─ USB stick for offline storage
        │
        ↓ (WiFi or BLE)
   [Data Sync]
        │
        ├─→ USB Stick (cross-correlation, validation)
        ├─→ Public APIs (weather, flight data)
        └─→ Local dashboard (daily/weekly graphs)
```

---

## Hardware Specification

### ESP32 Device

| Property | Value | Notes |
|----------|-------|-------|
| **Model** | ESP32-D0WDQ6 | 2 cores, 240 MHz |
| **RAM (SRAM)** | 520 KB | Shared program + data |
| **Flash** | 4 MB | Code + small data structures |
| **PSRAM** | None | No external PSRAM on this variant |
| **Power** | Li-ion/Li-Po battery with onboard charge circuit | No built-in sense pin; **external divider** BAT+→1MΩ→GPIO32→2MΩ→GND with 0.1µF tap→GND. Vbat = Vpin × 1.5 |
| **WiFi** | Yes (802.11 b/g/n, 2.4 GHz) | Active syncs only; off for low power |
| **Bluetooth** | Yes (BLE 5.0) | Alternative to WiFi; TBD which is primary |
| **I2C Pins** | SDA=GPIO16, SCL=GPIO17 | Shared bus for 3 sensors |
| **ADC1 Pins** | GPIO32 (battery), GPIO34 (soil), GPIO35 (HCHO), GPIO39 (CO) | WiFi-safe (ADC2 not usable with WiFi) |
| **I2S Pins** | GPIO26 (SCK), GPIO25 (WS), GPIO33 (SD) | Microphone input |

### Sensors (10 total)

| Sensor | Interface | Quantity | Raw Range / Units | Notes |
|--------|-----------|----------|-------------------|-------|
| **SEN66** (PM/RH-T/VOC/NOx/CO₂) | I²C 0x6B | 1 | PM: 0–1000 µg/m³, CO₂: 400–40000 ppm | ~1 Hz internal update; adaptive indices (VOC/NOx) need baseline settling (~hours) |
| **ADXL345** (3-axis accelerometer) | I²C 0x53 | 1 | ±4 g (configurable), ~3.9 mg/LSB | **Ground-rumble analysis (mic-style):** capture a short burst of samples, remove gravity (DC), report AC-magnitude RMS + peak as a "rumble level" — coarser than the mic FFT but the accelerometer analogue of it. Optional activity interrupt (INT1/GPIO27 unused currently) |
| **BH1750** (ambient light) | I²C 0x23 | 1 | 0–65536 lux | 1 lx resolution in HIGH_RES mode; ~120 ms per read |
| **SEN0564** (CO) | ADC GPIO39 (VN) | 1 | Raw mV 0–3300; Rs/R0 ratio qualitative | MEMS MOS; qualitative trend only; R0≈41.9 kΩ (clean air baseline) |
| **SEN0563** (HCHO/formaldehyde) | ADC GPIO35 | 1 | Raw mV 0–3300; Rs/R0 ratio; range 0–3 ppm qualitative | MEMS MOS; R0≈142.8 kΩ; cross-sensitive to other VOCs |
| **Capacitive soil moisture** | ADC GPIO34 | 1 | Raw mV (map via calibration); 0–100 % | Supply-sensitive; calibrated per unit (dry=2600 mV, wet=1200 mV empirically) |
| **Battery sense** | ADC GPIO32 | 1 | Raw mV 0–3300; Vbat = Vpin × 1.5 → 3.0–4.2 V | This board has **no built-in battery-sense pin**, so we use an **external divider**: BAT+→1MΩ→GPIO32→2MΩ→GND, with a 0.1µF cap (tap→GND) as the ADC charge reservoir (the ~0.67MΩ source impedance needs it). Ratio 2/3 → 4.2V reads 2.8V. Firmware stays configurable/disableable (`BATTERY_ENABLED`, `PIN_BATTERY_ADC`, `BAT_DIVIDER_FACTOR`, `BAT_CALIBRATED`). Until calibrated against a multimeter, raw mV is logged and V/% are flagged uncalibrated. |
| **INMP441** (I²S microphone) | I²S | 1 | 24-bit samples @ 44100 Hz; dBFS → SPL | Sensitivity −26 dBFS @ 94 dB SPL; noise floor ~33 dB(A); AOP 116 dB SPL |
| **Status LED** | GPIO13 | 1 | On-board indicator | For boot/connectivity feedback |

---

## Data Collection & Sampling Strategy

### Baseline Sampling (every 60 seconds)

All sensors read once per minute **unless adaptive sampling is triggered**.

| Sensor Group | Baseline Cadence | Notes |
|--------------|------------------|-------|
| SEN66 (PM, CO₂, VOC, NOx, T, RH) | 60 s | ~1 Hz internal; read every 60 s |
| ADXL345 (acceleration) | 60 s | Single read; optional event-based alternatives later |
| BH1750 (light) | 60 s | ~120 ms read latency |
| Gas sensors (CO, HCHO) | 60 s | Analog; quick read |
| Soil moisture | 60 s | Analog; quick read |
| Battery voltage | 60 s | Analog; quick read |
| INMP441 (microphone) | Every 10–30 s | ~512-sample FFT window (~12 ms @ 44100 Hz); summary stats (RMS, dBFS, SPL estimate, maybe octave bands) stored, not raw samples |

### Adaptive (Rapid) Sampling

When **any enabled sensor** exceeds its change threshold, **switch all sensors to 5–10 s cadence for T_hold seconds**, then return to baseline.

| Sensor | Trigger Condition | Change Threshold | T_hold | Active? |
|--------|-------------------|------------------|--------|---------|
| PM2.5 | Absolute jump | >10 µg/m³ | 300 s (5 min) | Yes (configurable) |
| CO₂ | Absolute jump | >200 ppm | 300 s | Yes (configurable) |
| VOC Index | Percentage | >30 % from last | 300 s | Yes (configurable) |
| NOx Index | Percentage | >30 % from last | 300 s | Yes (configurable) |
| Temperature | Absolute jump | >2 °C | 180 s | Yes (optional) |
| CO resistance (Rs) | Percentage | >20 % from R0 baseline | 300 s | Yes (optional; R0 needs calibration) |
| HCHO resistance | Percentage | >20 % from R0 baseline | 300 s | Yes (optional; R0 needs calibration) |
| Microphone (SPL) | Threshold | >70 dB SPL | 60 s | Yes (optional) |
| Light (lux) | Percentage | >50 % jump (e.g., day↔night) | 180 s | Yes (optional) |
| Acceleration (magnitude) | Threshold | >1.5 g (significant vibration) | 120 s | Yes (optional) |

**Notes:**
- Thresholds are **configurable per sensor** (stored in flash config struct).
- `T_hold` is the time to remain in rapid mode after last trigger.
- **Sound is always noisy** → likely will be excluded from adaptive triggers after analysis.
- **Adaptive trigger evaluation happens at baseline cadence** (60 s) to save power; rapid sampling begins next cycle.

### Data Record Format

Each record contains:
- **Timestamp** (Unix epoch, uint32_t, 4 bytes)
- **SEN66 fields** (6 floats, 24 bytes: PM1/PM2.5/PM4/PM10, VOC, NOx, CO₂ ppm, temp, RH)
  - Actually: PM1 + PM2.5 + PM4 + PM10 + CO₂ + VOC + NOx + T + RH = 9 floats = 36 bytes
- **ADXL345** (3 floats, 12 bytes: x, y, z m/s²)
- **BH1750** (1 float, 4 bytes: lux)
- **SEN0564 CO** (2 floats, 8 bytes: mV, Rs Ω)
- **SEN0563 HCHO** (2 floats, 8 bytes: mV, Rs Ω)
- **Soil moisture** (2 values, 8 bytes: mV, %)
- **Battery** (2 values, 8 bytes: mV, %)
- **INMP441 summary** (4 floats, 16 bytes: RMS dBFS, SPL estimate, A-weighted SPL, peak SPL)

**Total per record: ~120 bytes** (rough estimate; exact size TBD after implementation).

---

## Storage & Memory Management

### On-Device (ESP32)

**Problem:** 4 MB flash, 520 KB SRAM, no PSRAM. Must buffer data safely before syncing to PC.

**Solution:** Ring buffer in RAM + periodic flash writes

| Component | Size (approx) | Notes |
|-----------|---------------|-------|
| Code + libraries | ~500 KB | Arduino, WiFi, I2C, I2S, sensor drivers |
| SRAM available | ~20 KB (conservative estimate) | After code/stack; can buffer ~166 records at 120 bytes each |
| Flash available | ~500 KB | After code; for config, small log, maybe a few hundred records |

**Design approach:**
1. **In-memory ring buffer** (20 KB, ~166 records at 60 s baseline = ~2.8 hours of data).
2. **On power-loss or flush event:** write to SPIFFS (flash filesystem).
3. **On WiFi/BLE connection:** sync to PC.
4. **Goal:** lose **at most** the last ~2.8 hours if power dies suddenly; LittleFS/SPIFFS should handle crashes.

**Memory estimate:** Assuming baseline sampling (1 record/60 s):
- 1 hour: 60 records × 120 bytes ≈ 7.2 KB
- 24 hours: 1440 records ≈ 172.8 KB (fits on flash)
- 1 week: 10080 records ≈ 1.2 MB (fits on flash)

**With adaptive sampling (5-10 s when triggered):**
- If triggered 5 times/day for 5 min each → +25 extra records/day → ~3 KB/day overhead.
- Still feasible for ~1 week on 500 KB flash.

**Sync strategy:**
- ESP32 broadcasts "data ready" via WiFi/BLE.
- LattePanda pulls data over WiFi/BLE once per hour (or on demand).
- Delete from ESP32 after successful ACK from PC.
- If sync fails, accumulate until it succeeds (or flash fills).

---

### On PC (LattePanda)

**Storage:** USB stick (can be 32+ GB, no constraints).

**Data format:** SQLite database (recommended for later analysis)
- Single file: `air-monitor.db`
- Schema:
  ```sql
  CREATE TABLE readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp INTEGER NOT NULL UNIQUE,  -- Unix epoch
    source TEXT,                         -- 'esp32' or 'weather' or 'aircraft'
    data_json TEXT,                      -- JSON blob for flexible schema
    sync_status TEXT DEFAULT 'synced'    -- 'pending', 'synced', 'archived'
  );
  ```
- **Why JSON blob?** Avoids schema migrations; easy to add/remove sensor fields later.
- **Advantages:** ACID guarantees, queryable, lightweight, portable (single file on USB).
- **Disadvantages:** slower than flat CSV for huge datasets (but 1 year at 60 s = ~525k records ≈ manageable).

**Backup/validation:**
- On USB stick, also maintain a simple **CSV export** (one per month) as human-readable redundancy.
- On sync, validate: no gaps >5 min in timestamps, no duplicate timestamps, sensor values in expected ranges.

---

## Connectivity & Sync Strategy

### Problem Statement

- **Intermittent connectivity:** PC may not have internet for days; ESP32 may not have WiFi reliably (10 m away in insulated house).
- **Low power:** WiFi on continuously drains battery; prefer to turn off between syncs.
- **Need robustness:** minimize data loss when either device crashes or loses power.

### Proposed Solution: Dual-Protocol (WiFi + BLE)

**WiFi (primary, but intermittent):**
- ESP32 wakes periodically (every 1 hour?), turns on WiFi, posts data to LattePanda via HTTP POST.
- LattePanda runs a simple HTTP server on port 8080 (or fixed MQTT broker).
- If WiFi unavailable after 30 s, return to sleep.
- Power savings: WiFi on ~1–2 minutes/hour → ~70 mA × 2 min = ~2.3 mAh/hour awake; in sleep ~10 µA.

**BLE (fallback, lower power):**
- If WiFi fails, advertise readings via BLE.
- LattePanda scans BLE periodically (every 30 min?) and pulls data if available.
- BLE drains ~20–50 mA when active; negligible in sleep.

**Hybrid strategy:**
- Attempt WiFi first (faster, more reliable indoors/shorter range).
- Fall back to BLE if WiFi fails (lower power, can work through insulation better).
- Eventually: allow user to disable one or the other via config.

**Frequency:** Data sync once per 1–4 hours (TBD after power measurements).

---

## Operational Modes

### Mode 1: Home Air Quality Monitor

**Primary goal:** Understand how occupancy, ventilation, and devices affect indoor air quality.

**User interactions (on LattePanda PC):**
1. **Event button/checklist:** Mark events in real-time or post-hoc:
   - Door open/close
   - Ventilation on/off
   - Occupancy (someone home / gone)
   - Devices: heating on, PC in use, sound system playing
   - Sleep (going to bed)

2. **Dashboard queries:**
   - "Show CO₂, VOC, NOx over last 24 h with my events overlaid."
   - "When I used the sound system, what happened to PM2.5?"
   - "What's the correlation between door opens and humidity spike?"
   - Daily/weekly cycle view: morning CO₂ rise, afternoon slump, night levels.

3. **Recommendations engine** (future):
   - "Open window between 10–11 AM when outdoor PM is lowest" (requires weather API).
   - "Ventilation on for 30 min after cooking reduces peak formaldehyde by X%."

**Sensor focus:**
- **High priority:** CO₂, PM2.5, VOC, NOx, RH, T
- **Medium priority:** PM1/PM4/PM10 (finer aerosol data), HCHO, light (circadian rhythm marker)
- **Low priority:** CO (unlikely indoors unless appliance malfunction), soil moisture (not relevant), microphone (ambient noise reference)

**Data retention:** 1 year on USB stick; summarize to daily/hourly aggregates after 30 days (to save space).

---

### Mode 2: Aircraft Pollution Tracker

**Primary goal:** Correlate aircraft overhead (ADS-B) with local pollution, noise, and light anomalies.

**Setup:**
- RTL-SDR (software-defined radio) connected to LattePanda via USB.
- Decodes ADS-B signals from aircraft on 1090 MHz.
- Cross-references with OpenSky/Flightradar24 APIs for flight metadata (aircraft type, callsign, destination, etc.).

**Data collection (during this mode):**
- **All sensors continue at baseline/adaptive cadence.**
- **Aircraft stream:** ADS-B records (ICAO hex, position, altitude, speed, type) at ~1–5 Hz per visible aircraft.
- **Correlation:** When ADS-B aircraft is directly overhead (altitude < 500 m, lat/lon match ±1 km), **force rapid sampling** for all sensors.
- **Analysis questions:**
  - Which aircraft types cause the largest PM spike?
  - Is the spike correlated with fuel burn (heavy load indicator)?
  - Does light output vary by aircraft/time of day?
  - Does noise precede pollution increase, or coincide?
  - Seasonal patterns: does winter (denser air) show stronger correlations?

**Sensor focus:**
- **Critical:** PM2.5, NOx, microphone (SPL), light, timestamp alignment.
- **Secondary:** Temperature (density affects dispersion), CO₂ (not aircraft-specific, but background air mixing).

**Data retention:** 1 year; segment by aircraft ICAO/callsign for correlation analysis.

**External data sources:**
- **OpenSky Network API:** real-time + historical flights, free tier (anonymous flights available).
- **Flightradar24 API:** commercial, higher update rate; use for validation if needed.

---

## PC-Side (LattePanda) Architecture

### Software Stack (DECIDED — Python, cross-platform)

Chosen for portability across the user's machines (**macOS dev, Fedora laptop, Windows laptop, Debian LattePanda target**) with no native build step, and because pandas/scipy read SQLite directly for later analysis.

| Component | Technology | Purpose |
|-----------|-----------|---------|
| **Data collection + API** | **FastAPI + uvicorn** | One app: HTTP ingest from ESP32, REST query API, *and* WebSocket live-push |
| **Storage** | **SQLite (stdlib)** | Single file on USB stick; no server; ACID; pandas-readable |
| **Dashboard** | **Plotly.js + vanilla HTML/JS** | Interactive zoomable graphs, live updates over WebSocket, in-page query |
| **Weather fetch** | Python `requests` to KNMI/OpenWeather | Hourly; store in same SQLite (Phase 4) |
| **ADS-B decode** | `dump1090` / `readsb` (RTL-SDR) | Runs continuously in aircraft mode (Phase 3) |
| **Correlation engine** | Python (scipy, pandas) | Post-hoc analysis; generates reports |

**Why FastAPI over Flask:** native async WebSocket support (for live data) and automatic OpenAPI docs at `/docs`, with the same simplicity as Flask for the REST routes.

### Dashboard Features

1. **Real-time view:** Last 24 h of all sensors + events.
2. **Daily cycle:** Show median ± std dev for each hour of day (averaged over past week/month).
3. **Heatmap:** Time-of-day vs. day-of-week for CO₂, PM2.5, VOC (identify patterns).
4. **Event overlay:** User-marked events appear on graphs; show ±30 min window for impact analysis.
5. **Aircraft view** (mode-specific): Map of nearby flights, linked to PM/noise spikes.
6. **Export:** CSV/JSON snapshot for external analysis (R, Julia, Jupyter, etc.).

---

## Secret Configuration

**File:** `firmware/src/secrets.h` (already in .gitignore; copy from `secrets.example.h`)
- WiFi SSID, password
- Sensor calibration constants (R0 for gas sensors, BAT_FACTOR, soil calibration)
- (Later) API keys for weather/flight data, location coordinates (52.179722, 5.284722)

**PC-side:** `.env` file or config.json
- (Later) API keys for OpenSky/Flightradar24
- Database path
- location coordinates (same as ESP32)

---

## Known Unknowns / Assumptions to Validate

### Critical Path

| Item | Impact | Status | Owner |
|------|--------|--------|-------|
| **Battery divider calibration (GPIO32)** | Without it, battery % is off (1M/2M tolerances) | External divider built; measure Vbat vs raw mV, set `BAT_DIVIDER_FACTOR` (~1.5), then `BAT_CALIBRATED=1` | Arvid |
| **Power consumption model** | Affects battery life; shapes sync frequency | TBD; needs measurement | Dev |
| **WiFi range / signal strength** | Determines if WiFi is viable for 10 m + insulation | TBD; site survey | Arvid |
| **EEPROM/flash wear** | Frequent writes to flash may degrade it | TBD; research SPIFFS lifetime | Dev |
| **Gas sensor R0 baseline** | Without stable R0, CO/HCHO trends are meaningless | Needs ~1 h burn-in + clean air ref | Arvid |

### Implementation Decisions

| Item | Options | Recommendation | Rationale |
|------|---------|-----------------|-----------|
| **Ring buffer size** | 10 KB (80 records, ~1.3 h), 20 KB (166 records, ~2.8 h), 50 KB (416 records, ~7 h) | Start with 20 KB | Balances safety (lose <3 h) vs. RAM usage |
| **Baseline sampling interval** | 60 s, 30 s, 120 s | **60 s** | 60 s is reasonable; 30 s wastes power; 120 s loses real-time detail |
| **Rapid sampling cadence** | 1 s, 5 s, 10 s | **5–10 s** | 1 s overkill; 5 s catches ramps; 10 s is compromise |
| **Sync frequency** | Every 1 h, 4 h, 24 h | **Depends on power budget** | TBD after current measurements; likely 1–4 h range |
| **Data format on USB** | SQLite, CSV, both | **SQLite + monthly CSV export** | SQLite flexible; CSV is insurance + portability |
| **WiFi vs. BLE primary** | WiFi, BLE, dual | **WiFi primary + BLE fallback** | WiFi faster; BLE lower power; hybrid best |

---

## Implementation Plan (High-Level Roadmap)

### Phase 1: Baseline Data Collection (6–8 weeks)

1. **ESP32 firmware:**
   - Simple 60 s baseline sampling, store in ring buffer + SPIFFS.
   - WiFi sync to LattePanda (HTTP POST).
   - Battery measurement + reporting.
   - Status LED feedback.

2. **PC-side:**
   - HTTP server to receive data.
   - Store in SQLite.
   - Simple web dashboard (daily graph, last 7 days).
   - CSV export.

3. **Testing:**
   - Run continuously for 1–2 weeks indoors.
   - Measure battery life.
   - Validate sensor readings (spot-check vs. calibration refs).
   - No adaptive sampling yet; focus on reliability.

**Success criteria:** 1 week without data loss, <5% WiFi sync failures, dashboard shows sensible graphs.

### Phase 2: Adaptive Sampling & Analysis Tools (4–6 weeks)

1. **Adaptive triggers:**
   - Implement configurable thresholds per sensor.
   - Test with manual events (door open, cooking, etc.).
   - Measure power impact (should be minimal).

2. **Event logging:**
   - Simple button/web form on LattePanda to mark events.
   - Store event timestamps alongside sensor data.

3. **Correlation analysis:**
   - Python script to find CO₂ rise after door open, etc.
   - Generate weekly summary report.

**Success criteria:** Identify 2–3 reproducible correlations (e.g., "oven use → CO₂ spike").

### Phase 3: Aircraft Mode (8–12 weeks)

1. **RTL-SDR integration:**
   - Set up `dump1090` / `readsb` on LattePanda.
   - Decode ADS-B; store flight tracks in SQLite.
   - Integrate with OpenSky API for validation.

2. **Correlation with sensor data:**
   - When aircraft overhead, overlay on PM/noise graph.
   - Analyze patterns over 1–2 months.

**Success criteria:** Identify 1–2 aircraft types with measurable PM correlation.

### Phase 4: Weather Integration & Refinement (ongoing)

1. **Weather API:**
   - Fetch hourly data from KNMI / OpenWeather.
   - Store alongside sensor readings.

2. **Advanced analysis:**
   - Wind direction effect on PM (if outdoor source).
   - Seasonal patterns.

3. **Dashboard enhancements:**
   - Daily/weekly cycle heatmaps.
   - Forecast-correlation (e.g., "rain predicts lower PM tomorrow").

---

## Testing & Validation Plan

### Unit Testing

- [ ] Sensor read functions: verify output ranges and error handling.
- [ ] Ring buffer: test wrap-around, edge cases (full buffer, empty buffer).
- [ ] Adaptive trigger logic: unit tests with mock sensor values.
- [ ] Timestamp generation: check for overflows (uint32_t → year 2038 problem addressed).

### Integration Testing

- [ ] All sensors simultaneous reads under WiFi + I2C load.
- [ ] Ring buffer + SPIFFS writes under power stress.
- [ ] WiFi sync during rapid sampling.
- [ ] BLE fallback if WiFi unavailable.
- [ ] JSON serialization for all sensor combinations.

### Field Testing

- [ ] 1-week continuous run indoors (battery life, sync reliability, no crashes).
- [ ] 1 week outdoors (10 m away, insulation interference).
- [ ] Manual event correlation (open door, use appliance, record sensors).
- [ ] Spot-check sensor calibration vs. reference instruments.

### Data Quality

- [ ] No gaps >5 min in any sensor stream (goal: 95% uptime).
- [ ] No duplicate timestamps.
- [ ] All sensor values within physical bounds (e.g., RH 0–100, temp −10 to +50 °C).
- [ ] Battery % doesn't oscillate wildly (moving average to detect noise).

---

## Appendix: Sensor Performance Summary

From [`docs/SENSORS.md`](docs/SENSORS.md):

| Sensor | Absolute? | Uncertainty | Notes |
|--------|-----------|------------|-------|
| **SEN66 PM** | Yes | ±10 % | Ready to use; compare to WHO limits |
| **SEN66 CO₂** | Yes | ±50 ppm + 5 % | Use for ventilation tracking |
| **SEN66 VOC** | Relative | Index 100=baseline | Adaptive; first hours unstable |
| **SEN66 NOx** | Relative | Index 1=baseline | Sharp rises on NO₂ events; adaptive |
| **ADXL345** | Yes | ~3.9 mg/LSB | Good for vibration/motion logging |
| **BH1750** | Yes | Spectral response ~560 nm (human eye approx) | Good for daylight/night cycles |
| **SEN0564 CO** | No (qualitative) | R0 calibration per unit; ±10 % Rs curve | Track Rs/R0 ratio; useful for trends |
| **SEN0563 HCHO** | No (qualitative) | R0≈142 kΩ empirically; cross-sensitive | Track Rs/R0; useful for trends |
| **Soil moisture** | No (calibration per unit) | ±5 % after calibration | Supply-sensitive; local baseline required |
| **Battery** | Semi-absolute | BAT_FACTOR empirical (4.4 TBD) | After calibration, ~±5 % |
| **INMP441 SPL** | Semi-absolute | −26 dBFS @ 94 dB SPL; noise floor 33 dB(A) | Uncalibrated; calibrate against ref meter |

---

## Document Metadata

- **Created:** 2026-06-26
- **Last Updated:** 2026-06-26
- **Author:** Claude Code (user: Arvid)
- **Status:** DRAFT (awaiting feedback on assumptions)
- **Feedback Needed:** Battery pin calibration, power budget measurements, WiFi range test, gas sensor R0 burn-in

---

## Next Steps for the Next LLM

1. **Review this document:** Are assumptions clear? Any contradictions?
2. **Clarifications needed:** Once Phase 1 firmware is drafted, run power measurements to validate sync frequency.
3. **Begin Phase 1 implementation:** Start with basic 60 s sampling, ring buffer, WiFi sync.
4. **Create schema:** Design SQLite schema and JSON record format.
5. **Build PC-side HTTP server:** Simple Flask app to listen and store data.

**Questions for continuity:**
- Is the 20 KB ring buffer size sensible for this device's SRAM budget?
- Should we pre-allocate the ring buffer in PSRAM (not available) or stick to SRAM?
- Should the JSON blob in SQLite include all sensors, or split into separate tables per sensor type?
