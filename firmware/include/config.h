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
// 100 kHz standard mode: the Sensirion SEN66 uses clock-stretching and is unreliable
// in 400 kHz fast mode (corrupted frames → impossible values like RH 404 %, non-
// monotonic PM). 100 kHz is rock-solid for every sensor on this bus.
#define I2C_FREQ_HZ          100000

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
// INMP441 SPL anchor: SPL ≈ dBFS + MIC_DBFS_TO_SPL_DB.
// Field-calibrated 2026-06-28 against a phone SPL meter (two passes):
//   pass 1: firmware 69 vs ref 30.4  → offset 81.4
//   pass 2: our 5-min LAeq 25.4 vs ref 29 → +3.6  → offset 85.0
// LAmax already tracked the phone closely; this aligns the LAeq average too.
// Re-check against a class-2 meter for absolute accuracy when possible.
#define MIC_DBFS_TO_SPL_DB   85.0f
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
// Empirical: device (ESP32 + WiFi) brownouts at ~3.42-3.45 V under load.
// Raised from 3.0 V (which showed 35% remaining at brownout — misleading).
#define BAT_EMPTY_V           3.45f
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
#define SAMPLE_BASELINE_MS   10000UL    // 60 s

// --- Accelerometer vibration capture ---
// Poll at ~400 Hz (I2C overhead + gap ≈ 2.5 ms/sample), capture 512 samples (~1.3 s).
// FFT gives 1/3-octave bands 4–125 Hz; integration gives peak particle velocity (PPV).
#define ACCEL_FFT_SAMPLES     512       // samples per window; must be power of 2
#define ACCEL_FFT_GAP_US      1500      // µs of padding between samples (I2C adds ~1 ms more)
#define ACCEL_NBANDS          6         // 1/3-oct bands: 4, 8, 16, 31.5, 63, 125 Hz

// --- Binary FIFO ring buffer on flash (LittleFS) ---
// Fixed-size packed records (see record.h, RECORD_SIZE=71). Drop-oldest when
// full. Stored as APPEND-ONLY segment files under QUEUE_DIR, never one big
// preallocated file: a random in-place write to a large LittleFS file triggers
// a full-file copy-on-write (CTZ skip-list) needing free space >= the file size
// and crashes the allocator (NOSPC surfaced as IntegerDivideByZero). See
// ringstore.cpp for the segment layout. seq -> (segment, offset) is implicit, so
// head/tail are recovered by scanning the dir; only the synced cursor persists.
#define QUEUE_DIR            "/q"             // directory holding /q/<hex>.seg files
#define QUEUE_CURSOR_PATH    "/q/cursor"      // double-buffered, CRC'd synced boundary
#define QUEUE_FMT_PATH       "/q/fmt"         // record-format signature guarding reflash reinterpretation
#define SEG_RECORDS          500U             // records per segment (500*71 = ~35.5 KB ~= 9 blocks)
#define RING_CAPACITY        12000U           // logical capacity ~= ~8.3 days @ 60s (24 segments)
#define SYNC_BATCH_MAX       20              // records POSTed per HTTP request
#define SYNC_BATCH_MAX_BYTES 4096            // ...or this many bytes, whichever first

// Legacy queue paths — removed on boot if present (no migration needed).
#define LEGACY_QUEUE_PATH    "/queue.ndjson"
#define LEGACY_CURSOR_PATH   "/cursor.txt"
#define RING_PATH            "/ring.bin"      // old single-file ring (deleted on boot)
#define RING_META_PATH       "/ring.meta"     // old ring metadata   (deleted on boot)

// --- Duty-cycled sync + modes ---
// NORMAL: offload every SYNC_ATTEMPT_INTERVAL_MS, or early once SYNC_THRESHOLD_RECORDS
//         are buffered. WiFi stays on (mains-powered), so a short interval is cheap and
//         keeps the dashboard ~live and makes pushed config/commands land within ~1 min.
// TESTING: WiFi stays on, every record pushed live the instant it's sampled.
// BOOT_WINDOW: after every power-on WiFi is up + syncs every 5 s so a dashboard
//              command (e.g. enter testing / change interval) lands immediately.
#define SYNC_ATTEMPT_INTERVAL_MS  (10UL * 1000UL)           // 10 s — near-live uploads
#define SYNC_THRESHOLD_RECORDS    20                         // early-trigger for bursts
#define BOOT_WINDOW_MS            (5UL * 60UL * 1000UL)      // 5 min

// --- Connection robustness / self-healing ---
// The station is unattended, so it must recover from a wedged link on its own.
// All buffered data is durable on flash, so a reboot loses nothing — making
// "reboot when stuck" a cheap last resort rather than a data-loss event.
//   - After WIFI_HARD_CYCLE_FAIL consecutive failed sync attempts, fully power-
//     cycle the radio (clears a stuck DHCP/TCP stack that still reports associated).
//   - After STALL_REBOOT_MS with no acknowledged sync, ESP.restart() and resume
//     from the persisted ring cursor.
//   - The task watchdog reboots if a blocking call (HTTP/mDNS/I2S) hangs the loop.
#define WIFI_HARD_CYCLE_FAIL      4                          // failed syncs before radio power-cycle
#define STALL_REBOOT_MS           (15UL * 60UL * 1000UL)     // 15 min with no good sync -> reboot
#define WDT_TIMEOUT_S             60                          // loop watchdog timeout (> one full sync session)
#define HTTP_TIMEOUT_MS           5000                        // per-POST timeout (was 8000)

// --- Split-rate + adaptive cadence (cadence.h) ---
// The mic captures continuously (~1.3 s/capture); these knobs decide which captures
// get *stored* and how often the slow sensors are re-read. Aircraft flyovers are
// near-single-sample even at 10 s, so we densify storage when the level moves fast
// and decimate when quiet. quiet_store_ms defaults to the server-set poll_interval_ms
// so the dashboard's existing cadence knob still controls the baseline.
#define SLOW_INTERVAL_MS          (3UL * 60UL * 1000UL)       // slow sensors re-read every 3 min...
#define SLOW_INTERVAL_DENSE_MS    (20UL * 1000UL)             // ...or every 20 s while densified
#define NOISE_DENSIFY_DELTA_DBA   6.0f                        // |Δ LAeq| between captures that arms densify
#define DENSIFY_HOLD_MS           (30UL * 1000UL)             // densified window length after each trigger

// --- POWER_SAVING mode (battery operation) ---------------------------------
// A third operating mode alongside NORMAL/TESTING, selected from the dashboard and
// pushed via the /ingest reply like the others. Everything below ONLY takes effect
// in MODE_POWER_SAVING — NORMAL/TESTING keep their mains-powered behaviour untouched.
//
// The radio dominates average current, so power saving duty-cycles WiFi (off between
// syncs), light-sleeps the CPU between mic captures, gates the analog gas-sensor
// heaters and the SEN66 fan/laser through their warm-up, and drops the mic sample
// rate to halve FFT CPU. The accelerometer is read in the same windows as the mic
// (and sleeps in between) — no separate poll. See DESIGN.md "Phase 4 / power".
//
// Sync less often than NORMAL (radio up ~once per interval, then off). Trades
// dashboard liveness + config-push latency for battery life.
#define POWER_SAVE_SYNC_INTERVAL_MS   (5UL * 60UL * 1000UL)   // drain ring every 5 min
// Inter-capture light sleep: after each ~1.3 s mic capture, light-sleep this long
// (CPU + radio modem suspended, RAM retained) before the next capture. Larger =
// less power but sparser noise sampling. 0 keeps the loop free-running (no sleep).
#define POWER_SAVE_CAPTURE_GAP_MS     (5UL * 1000UL)
// Mic sample rate while power-saving (vs I2S_SAMPLE_RATE_HZ). 16 kHz covers bands up
// to ~4 kHz (Nyquist 8 kHz) and ~halves FFT CPU; aircraft energy is low-frequency.
#define POWER_SAVE_MIC_RATE_HZ        16000

// --- Gas-sensor heater gating (analog DFRobot Fermion MEMS boards) ----------
// The MEMS heaters draw continuously; in power-saving we power them through a
// low-side MOSFET on this GPIO, warm up, read, then cut power. Set HIGH (heaters on)
// continuously in NORMAL/TESTING. NOTE: the MOSFET is not on the current board — pin
// is driven harmlessly until the board is rebuilt. GPIO4 is broken out and free.
#define PIN_GAS_HEATER_EN     4
#define GAS_HEATER_WARMUP_MS  (30UL * 1000UL)   // MEMS heater stabilisation before a read

// --- SEN66 duty-cycling -----------------------------------------------------
// In power-saving the SEN66 is stopped (fan + laser off, idle current) between slow
// reads; before each read it is restarted and given this long to spin up + stabilise
// the PM measurement. Datasheet warm-up for stable PM is several seconds.
#define SEN66_WARMUP_MS       (10UL * 1000UL)

// --- Time ---
#define NTP_SERVER_1         "pool.ntp.org"
#define NTP_SERVER_2         "time.google.com"
// How often to (re)attempt NTP while the clock is still unsynced. Each attempt briefly
// blocks the loop (~2 s) and re-inits SNTP, so without this throttle an offline station
// stalls every loop until it adopts server time. Once synced, no attempts are made.
#define NTP_RETRY_INTERVAL_MS (60UL * 1000UL)
// Epoch sanity floor: a timestamp below this means NTP never succeeded.
#define EPOCH_VALID_AFTER    1735689600UL  // 2025-01-01; records before this are "unsynced time"

// --- Device identity (shows up in the database `device` column) ---
#define DEVICE_ID            "air-monitor-01"
