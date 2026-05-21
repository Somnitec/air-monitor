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
// ADXL345 0x53 | BH1750 0x23 | BME280 0x76 | SEN66 0x6B
// DS3231  0x68 | DFRobot CO 0x74 (default) | DFRobot HCHO 0x75 (set via dip)
// IMPORTANT: the two DFRobot gas boards ship at the same default address —
// set them to different addresses via their on-board DIP/jumper before wiring.
#define PIN_I2C_SDA          16
#define PIN_I2C_SCL          17
#define I2C_FREQ_HZ          400000

// ---- Wi-Fi / time sync ----
#define WIFI_SSID            "YOUR_SSID"
#define WIFI_PASSWORD        "YOUR_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define NTP_POOL             "pool.ntp.org"
#define TZ_INFO              "UTC0"

// ---- I2S microphone (INMP441) ----
// Mic L/R pin: tie to GND for left channel (we read one mono channel).
#define PIN_I2S_BCLK         26
#define PIN_I2S_LRCLK        25
#define PIN_I2S_DIN          33
#define I2S_SAMPLE_RATE_HZ   16000

// ---- SD card (VSPI defaults) ----
#define PIN_SD_SCK           18
#define PIN_SD_MISO          19
#define PIN_SD_MOSI          23
#define PIN_SD_CS             5

// ---- Analog (ADC1) ----
// All analog sensors must live on ADC1 (GPIO32-39) so they stay readable
// while Wi-Fi is active. Use 11 dB attenuation for 0–~3.3 V input range.
#define PIN_SOIL_ADC         32   // Keyestudio capacitive soil moisture
#define PIN_GAS_CO_ADC       34   // DFRobot Gravity CO  (analog out)
#define PIN_GAS_HCHO_ADC     35   // DFRobot Gravity HCHO (analog out)
#define GAS_ADC_OVERSAMPLE   32   // average N samples per reading to fight ADC noise

// ---- Interrupts ----
// ADXL345 INT1 → wake/log on rumble events (configure FIFO + activity int).
#define PIN_ADXL_INT1        27

// ---- Status ----
#define PIN_STATUS_LED        2   // many boards have on-board LED here

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
