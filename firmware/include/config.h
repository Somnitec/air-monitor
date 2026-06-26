#pragma once

// =========================================================================
// Pin map — ESP32-D0WDQ6 Tinytronics dev board (26-pin, Li-ion charger).
// Board exposes:
//   Left  : VP(36) VN(39) EN 34 35 32 33 25 26 27 14 12 GND
//   Right : 3V3 22 19 23 18 5 17 16 4 0 2 15 13
//
// NOTE: GPIO21 (default I2C SDA on ESP32) is NOT broken out — remapped below.
// NOTE: GPIO12 is MTDI strapping pin — flashed at boot, do NOT pull high.
// NOTE: GPIO0/2/5/15 are also strapping pins — avoid pulling unusually at boot.
// NOTE: ADC2 pins (0,2,4,12-15,25-27) can't be read while Wi-Fi is active —
//       analog sensors use ADC1 only (GPIOs 32-39).
// =========================================================================

// ---- I2C bus (sensors + RTC) ----
// SEN66 0x6B | ADXL345 0x53 | BH1750/GY-30 0x23 | (optional) BME280 0x76, DS3231 0x68
// All distinct — no address conflicts. The DFRobot CO/HCHO sensors here are the
// ANALOG Fermion MEMS boards (SEN0564 / SEN0563), not the I2C gas boards.
#define PIN_I2C_SDA          16
#define PIN_I2C_SCL          17
#define I2C_FREQ_HZ          400000

// Expected I2C addresses (used by the boot self-test).
#define ADDR_SEN66           0x6B
#define ADDR_ADXL345         0x53
#define ADDR_BH1750          0x23
#define ADDR_BME280          0x76   // SDO→GND; would be 0x77 with SDO→VCC
#define ADXL345_DEVID        0xE5   // DEVID register (0x00) must read this
#define BME280_CHIPID        0x60   // reg 0xD0 must read this (BME280, not BMP280=0x58)

// ---- Wi-Fi / time sync ----
// Real values live in src/secrets.h (gitignored). These #ifndef fallbacks let the
// file compile if secrets.h is missing, but sync won't work until you fill it in.
// Copy src/secrets.example.h -> src/secrets.h and edit.
#ifndef WIFI_SSID
#define WIFI_SSID            "YOUR_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD        "YOUR_PASSWORD"
#endif
#ifndef SYNC_HOST
#define SYNC_HOST            "192.168.1.50"   // LattePanda IP running the FastAPI server
#endif
#ifndef SYNC_PORT
#define SYNC_PORT            8000
#endif
#ifndef SYNC_PATH
#define SYNC_PATH            "/ingest"
#endif
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define NTP_POOL             "pool.ntp.org"
#define TZ_INFO              "UTC0"

// ---- I2S microphone (INMP441) ----
// Mic L/R pin: tie to GND for left channel (we read one mono channel).
// SCK(BCLK)=26, WS(LRCLK)=25, SD(DOUT)=33. See docs/SENSORS.md for the
// dBFS->SPL anchor and A-weighting math.
#define PIN_I2S_BCLK         26
#define PIN_I2S_LRCLK        25
#define PIN_I2S_DIN          33
// 48 kHz gives clean coverage of all octave bands up to 8 kHz (Nyquist 24 kHz).
// Drop to 16000 if you only care about levels below ~4 kHz and want less CPU.
#define I2S_SAMPLE_RATE_HZ   48000
#define MIC_FFT_SAMPLES      2048    // power of two; ~43 ms window @ 48 kHz
// INMP441 sensitivity anchor: -26 dBFS @ 94 dB SPL  =>  SPL ~= dBFS + 120.
// UNCALIBRATED — calibrate this offset against a reference meter for absolute dB.
#define MIC_DBFS_TO_SPL_DB   120.0f
#define MIC_NOISE_FLOOR_DBA  33.0f   // datasheet equiv. input noise; can't read quieter

// ---- SD card (VSPI defaults) ----
#define PIN_SD_SCK           18
#define PIN_SD_MISO          19
#define PIN_SD_MOSI          23
#define PIN_SD_CS            13   // (SD unused in Phase 1; GPIO13 also drives the status LED)

// ---- Analog (ADC1) ----
// All analog sensors must live on ADC1 (GPIO32-39) so they stay readable
// while Wi-Fi is active. Use 11 dB attenuation for 0–~3.3 V input range.
#define PIN_SOIL_ADC         34   // generic capacitive soil moisture (analog out)
#define PIN_GAS_CO_ADC       39   // DFRobot Fermion MEMS CO  SEN0564 (VN pin)
#define PIN_GAS_HCHO_ADC     32   // DFRobot Fermion MEMS HCHO SEN0563 (analog out)
                                  // moved 32->35: GPIO32 now hosts the battery divider.
                                  // GPIO35 is input-only — fine for a sensor analog output.
// ---- Battery sense (EXTERNAL divider — this board has no built-in sense pin) ----
// External divider (user-built):  BAT+ --[1M]--+--[2M]-- GND ,  +--[0.1uF 104]-- GND
//                                              |
//                                            GPIO35 (ADC1_CH7)
// Ratio = R2/(R1+R2) = 2/3, so Vpin = Vbat*0.667 (4.2V -> 2.8V, within ADC range).
// Reconstruct Vbat = Vpin * (R1+R2)/R2 = Vpin * 1.5  => BAT_DIVIDER_FACTOR = 1.5.
// The 0.1uF cap is REQUIRED: the ~0.67M source impedance is far above what the
// ADC sample-hold likes; the cap is its charge reservoir (RC ~ 67ms, also filters).
#define BATTERY_ENABLED         1     // 0 = skip battery entirely (logs nothing)
#define PIN_BATTERY_ADC        35     // external 1M/2M divider tap 
#define BAT_DIVIDER_FACTOR    1.5f    // verified: 4.20V/2804mV=1.498, 4.05V/2690mV=1.506 -> 1.50
#define BAT_CALIBRATED          1     // verified against multimeter (two points, both ~1.50).
                                      // While 0: raw mV is logged; V/% are flagged uncalibrated.
#define BAT_FULL_V            4.2f
#define BAT_EMPTY_V           3.0f
#define GAS_ADC_OVERSAMPLE   32   // average N samples per reading to fight ADC noise
#define GAS_CO_RL_OHMS       4700.0f  // RL on SEN0564 CO board
#define GAS_HCHO_RL_OHMS    10000.0f  // RL on SEN0563 HCHO board (different!)
#define GAS_VCC_MV           3300.0f  // divider supply; set to actual AOUT rail

// Analog "present?" heuristic for the self-test: a floating ADC pin reads near 0
// or pinned to the rail. We flag a reading as plausibly-connected if it sits
// inside this window (in mV). Tune to your sensors' real idle voltages.
#define ANALOG_PRESENT_MIN_MV   60
#define ANALOG_PRESENT_MAX_MV   3200

// Soil calibration endpoints (mV). Measure yours: V_dry in air, V_wet in water.
#define SOIL_DRY_MV          2600
#define SOIL_WET_MV          1200

// ---- Interrupts ----
// ADXL345 INT1 → wake/log on rumble events (configure FIFO + activity int).
#define PIN_ADXL_INT1        27

// ---- Status ----
#define PIN_STATUS_LED        2   // on-board LED (tied to charge circuit on this board — not reliable)
#define PIN_EXT_LED          13   // external LED: GPIO13 → 330Ω → LED → GND

// =========================================================================
// Sample cadence (ms) — tune later, these are reasonable defaults.
// =========================================================================
#define SAMPLE_MS_BME280     30000   // env. drifts slowly
#define SAMPLE_MS_BH1750     10000
#define SAMPLE_MS_SEN66       1000   // SEN66 internal cycle is ~1 Hz
#define SAMPLE_MS_GAS         5000   // CO + HCHO
#define SAMPLE_MS_SOIL       60000
#define SAMPLE_MS_ADXL_POLL    100   // or use INT for event-driven
#define SAMPLE_MS_MIC_RMS     1000   // RMS window for noise level

// =========================================================================
// Logging
// =========================================================================
#define LOG_FILENAME_FMT     "/log_%04d%02d%02d.csv"   // daily file via RTC
#define LOG_FLUSH_EVERY_N    10                         // flush SD every N rows

// =========================================================================
// Phase 1 — baseline data collection, durable queue, WiFi sync
// (See DESIGN.md "Implementation Plan / Phase 1".)
// =========================================================================

// --- Baseline sampling cadence ---
// One full record of every sensor at this interval. Adaptive (5–10 s) sampling
// is Phase 2; Phase 1 is fixed-cadence on purpose to gather a clean baseline.
#define SAMPLE_BASELINE_MS   60000UL    // 60 s

// --- Accelerometer ground-rumble capture (mic-style, coarser) ---
// Capture a short burst, remove gravity (DC), report AC-magnitude RMS + peak.
#define ACCEL_RUMBLE_SAMPLES   128      // samples per rumble window
#define ACCEL_RUMBLE_GAP_US   1500      // ~us between samples (~660 Hz attempt; I2C-limited)

// --- Durable queue on flash (LittleFS) ---
// Records are appended as newline-delimited JSON (NDJSON). A persisted byte
// cursor marks how far the PC has acknowledged. Survives power loss.
#define QUEUE_PATH           "/queue.ndjson"
#define QUEUE_CURSOR_PATH    "/cursor.txt"
#define QUEUE_COMPACT_BYTES  (64 * 1024)   // once fully-synced and bigger than this, truncate
#define SYNC_BATCH_MAX        20           // records POSTed per HTTP request
#define SYNC_BATCH_MAX_BYTES  4096         // ...or this many bytes, whichever first

// --- Time ---
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"
// Epoch sanity floor: a timestamp below this means NTP never succeeded.
#define EPOCH_VALID_AFTER    1735689600UL  // 2025-01-01; records before this are "unsynced time"

// --- Device identity (shows up in the database `device` column) ---
#define DEVICE_ID            "air-monitor-01"
