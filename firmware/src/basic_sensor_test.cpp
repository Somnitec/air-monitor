#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <SensirionI2cSen66.h>
#include <Adafruit_ADXL345_U.h>
#include <BH1750.h>
#include "../../secrets.h"

// ---------------- Pin map ----------------
// Matches the working assignments from basic_connectivity_test.cpp.
// See docs/SENSORS.md for interpretation notes on every sensor.

#define I2C_SDA             16
#define I2C_SCL             17

#define I2S_SCK_PIN         26
#define I2S_WS_PIN          25
#define I2S_SD_PIN          33
#define I2S_SAMPLE_RATE     44100

#define PIN_HCHO            35   // SEN0563 Fermion MEMS HCHO (moved 32->35)
#define PIN_CO              39   // SEN0564 Fermion MEMS CO  (VN)
#define PIN_SOIL            34   // capacitive soil moisture
#define PIN_BATTERY         32   // external divider BAT+→1M→GPIO32→2M→GND (+0.1µF tap→GND)

// External status LED: GPIO13 → 330Ω → LED anode → cathode → GND
// GPIO12 is MTDI strapping pin (avoid); GPIO13 is safe.
#define PIN_LED             13

// MEMS gas sensor load resistors — CO and HCHO boards use different values!
#define CO_RL_OHMS          4700.0f   // SEN0564 CO board
#define HCHO_RL_OHMS       10000.0f   // SEN0563 HCHO board
#define GAS_VCC_MV          3300.0f

// Soil calibration endpoints — measure yours in air and in water.
#define SOIL_DRY_MV         2600
#define SOIL_WET_MV         1200

// INMP441 sensitivity anchor: -26 dBFS at 94 dB SPL  →  SPL ≈ dBFS + 120.
#define MIC_SPL_OFFSET_DB   120.0f
#define MIC_SAMPLES         512     // ~12 ms window at 44100 Hz

// I2C addresses
#define ADDR_SEN66          0x6B
#define ADDR_ADXL345        0x53
#define ADDR_BH1750         0x23
#define ADXL_DEVID_EXPECTED 0xE5

// ---------------- Globals ----------------

static SensirionI2cSen66         sen66;
static Adafruit_ADXL345_Unified  adxl(12345);
static BH1750                    bh1750(ADDR_BH1750);

static bool hasSen66  = false;
static bool hasAdxl   = false;
static bool hasBh1750 = false;
static bool hasMic    = false;

// ---------------- Helpers ----------------

// Oversample an ADC1 pin (N=32) and return the average in millivolts.
static uint32_t readMv(int pin, int n = 32) {
    uint32_t acc = 0;
    for (int i = 0; i < n; i++) acc += analogReadMilliVolts(pin);
    return acc / n;
}

// Fermion MEMS gas board: compute sensor resistance Rs from output voltage.
// AOUT = Vcc * RL / (Rs + RL)  →  Rs = RL * (Vcc - Vout) / Vout
// RL differs between CO (4.7 kΩ) and HCHO (10 kΩ) boards.
static float gasRsOhms(uint32_t vout_mv, float rl_ohms) {
    if (vout_mv < 5) return INFINITY;
    return rl_ohms * (GAS_VCC_MV - (float)vout_mv) / (float)vout_mv;
}

static bool i2cAck(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)addr, 1);
    return Wire.available() ? Wire.read() : 0xFF;
}

// ---------------- Sensor init ----------------

static void initSensors() {
    Serial.println("\n--- Sensor init ---");

    // SEN66
    if (i2cAck(ADDR_SEN66)) {
        sen66.begin(Wire, ADDR_SEN66);
        hasSen66 = (sen66.startContinuousMeasurement() == 0);
        Serial.printf("  SEN66  (0x6B): %s\n", hasSen66 ? "OK" : "FAIL (start error)");
    } else {
        Serial.println("  SEN66  (0x6B): not found");
    }

    // ADXL345
    if (i2cAck(ADDR_ADXL345)) {
        uint8_t devid = i2cReadReg(ADDR_ADXL345, 0x00);
        if (devid == ADXL_DEVID_EXPECTED && adxl.begin(ADDR_ADXL345)) {
            adxl.setRange(ADXL345_RANGE_4_G);
            hasAdxl = true;
            Serial.println("  ADXL345(0x53): OK");
        } else {
            Serial.printf("  ADXL345(0x53): FAIL (DEVID=0x%02X)\n", devid);
        }
    } else {
        Serial.println("  ADXL345(0x53): not found");
    }

    // BH1750
    if (i2cAck(ADDR_BH1750)) {
        hasBh1750 = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, ADDR_BH1750);
        Serial.printf("  BH1750 (0x23): %s\n", hasBh1750 ? "OK" : "FAIL");
    } else {
        Serial.println("  BH1750 (0x23): not found");
    }

    // INMP441 — I2S
    {
        i2s_config_t cfg = {
            .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
            .sample_rate          = I2S_SAMPLE_RATE,
            .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
            // L/R pin tied to GND → mic outputs in the LEFT I2S slot (WS low),
            // but ESP32 driver maps that to ONLY_RIGHT. Counterintuitive but correct.
            .channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
            .dma_buf_count        = 4,
            .dma_buf_len          = 128,
            .use_apll             = false,
            .tx_desc_auto_clear   = false,
            .fixed_mclk           = 0,
        };
        i2s_pin_config_t pins = {
            .bck_io_num   = I2S_SCK_PIN,
            .ws_io_num    = I2S_WS_PIN,
            .data_out_num = I2S_PIN_NO_CHANGE,
            .data_in_num  = I2S_SD_PIN,
        };
        hasMic = (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) == ESP_OK &&
                  i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK);
        Serial.printf("  INMP441 (I2S): %s\n", hasMic ? "OK" : "FAIL");
    }

    Serial.println();
}

// ---------------- Sensor reads ----------------

static void readSen66() {
    float pm1, pm25, pm4, pm10, t, rh, voc, nox;
    uint16_t co2;
    if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, t, rh, voc, nox, co2) != 0) {
        Serial.println("SEN66: read error (data not ready yet?)");
        return;
    }
    Serial.println("┌─ SEN66 ─────────────────────────────────────────");
    Serial.printf( "│  PM1=%.1f  PM2.5=%.1f  PM4=%.1f  PM10=%.1f µg/m³\n",
                   pm1, pm25, pm4, pm10);
    Serial.printf( "│  CO₂=%u ppm  (>1000 = poor ventilation)\n", co2);
    Serial.printf( "│  VOC index=%.0f  (100=recent baseline; >100=elevated)\n", voc);
    Serial.printf( "│  NOx index=%.0f  (1=baseline; rises with NO₂ events)\n", nox);
    Serial.printf( "│  T=%.2f °C   RH=%.1f %%\n", t, rh);
    Serial.println("└──────────────────────────────────────────────────");
}

static void readAdxl() {
    sensors_event_t e;
    adxl.getEvent(&e);
    float x = e.acceleration.x;
    float y = e.acceleration.y;
    float z = e.acceleration.z;
    float mag = sqrtf(x*x + y*y + z*z);
    // Convert m/s² to g (1 g = 9.80665 m/s²)
    Serial.println("┌─ ADXL345 ───────────────────────────────────────");
    Serial.printf( "│  X=%.3f  Y=%.3f  Z=%.3f m/s²\n", x, y, z);
    Serial.printf( "│  X=%.3f  Y=%.3f  Z=%.3f g\n",
                   x/9.807f, y/9.807f, z/9.807f);
    Serial.printf( "│  |a|=%.3f m/s²  (≈%.3f g; ~1g when still)\n", mag, mag/9.807f);
    Serial.println("└──────────────────────────────────────────────────");
}

static void readBh1750() {
    float lux = bh1750.readLightLevel();
    Serial.println("┌─ BH1750 ────────────────────────────────────────");
    Serial.printf( "│  Light = %.1f lux\n", lux);
    // Context: overcast outdoor ~1000 lx, office ~300-500 lx, night <1 lx
    const char* context = lux < 1   ? "very dark / night"
                        : lux < 50  ? "dim room"
                        : lux < 300 ? "indoor artificial light"
                        : lux < 1000? "bright indoor / overcast outdoor"
                                    : "direct daylight";
    Serial.printf( "│  → %s\n", context);
    Serial.println("└──────────────────────────────────────────────────");
}

static void readGas() {
    uint32_t co_mv   = readMv(PIN_CO);
    uint32_t hcho_mv = readMv(PIN_HCHO);
    float co_rs   = gasRsOhms(co_mv,   CO_RL_OHMS);
    float hcho_rs = gasRsOhms(hcho_mv, HCHO_RL_OHMS);

    Serial.println("┌─ Gas sensors (qualitative MEMS MOS) ────────────");
    Serial.printf( "│  CO   (SEN0564, RL=4.7kΩ): %4u mV  Rs=%.0f Ω\n", co_mv, co_rs);
    Serial.printf( "│  HCHO (SEN0563, RL=10kΩ):  %4u mV  Rs=%.0f Ω\n", hcho_mv, hcho_rs);
    Serial.println("│  Spec: CO Rs/R0 ≤ 0.33 at 150 ppm | HCHO Rs/R0 ≤ 0.56 at 0.4 ppm");
    Serial.println("│  Record Rs in clean air as R0, then track Rs/R0 over time.");
    Serial.println("└──────────────────────────────────────────────────");
}

static void readSoil() {
    uint32_t mv = readMv(PIN_SOIL);
    // Linear map: dry end → 0%, wet end → 100%.
    // Calibrate SOIL_DRY_MV (sensor in air) and SOIL_WET_MV (in water).
    float pct = 100.0f * (float)(SOIL_DRY_MV - (int)mv)
                       / (float)(SOIL_DRY_MV - SOIL_WET_MV);
    pct = constrain(pct, 0.0f, 100.0f);

    Serial.println("┌─ Soil moisture ──────────────────────────────────");
    Serial.printf( "│  %u mV → %.0f %% moisture\n", mv, pct);
    Serial.printf( "│  (calibration: dry=%d mV, wet=%d mV)\n",
                   SOIL_DRY_MV, SOIL_WET_MV);
    Serial.println("└──────────────────────────────────────────────────");
}

static void readBattery() {
    uint32_t mv_pin = readMv(PIN_BATTERY);
    // R2=2.5MΩ confirmed. R1 unknown (diode in series, reading was still rising).
    // Calibrate: read raw_mV below, measure Vbat with a multimeter, then:
    //   BAT_FACTOR = Vbat_measured / (raw_mV / 1000.0)
    // External 1M/2M divider → nominal 1.5; calibrate to absorb resistor tolerances.
    const float BAT_FACTOR = 1.5f;   // <-- replace with your calibrated value
    float vbat = mv_pin / 1000.0f * BAT_FACTOR;
    float pct  = constrain((vbat - 3.0f) / (4.2f - 3.0f) * 100.0f, 0.0f, 100.0f);

    Serial.println("┌─ Battery ────────────────────────────────────────");
    Serial.printf( "│  Raw pin%d = %u mV  →  %.2f V est  (~%.0f %%)\n",
                   PIN_BATTERY,
                   mv_pin, vbat, pct);
    Serial.printf( "│  To calibrate: BAT_FACTOR = Vbat_multimeter / %.4f\n",
                   mv_pin / 1000.0f);
    Serial.println("│  4.2V=full  3.7V=nominal  3.0V=empty");
    Serial.println("└──────────────────────────────────────────────────");
}

static void readMic() {
    static int32_t buf[MIC_SAMPLES];
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, buf, sizeof(buf), &bytesRead,
                             pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytesRead == 0) {
        Serial.println("INMP441: I2S read error");
        return;
    }
    int n = bytesRead / sizeof(int32_t);

    // INMP441 packs a 24-bit sample in the top bits of the 32-bit slot.
    // Remove DC, compute RMS.
    double mean = 0;
    for (int i = 0; i < n; i++) mean += (int32_t)(buf[i] >> 8);
    mean /= n;

    double sumsq = 0;
    bool clipping = false;
    for (int i = 0; i < n; i++) {
        double s = (int32_t)(buf[i] >> 8) - mean;
        sumsq += s * s;
        if (abs((int32_t)(buf[i] >> 8)) > 8000000) clipping = true;
    }
    float rms      = sqrtf((float)(sumsq / n));
    float fullScale = 8388608.0f;                        // 2^23
    float dBFS     = (rms > 0) ? 20.0f * log10f(rms / fullScale) : -120.0f;
    float splEst   = dBFS + MIC_SPL_OFFSET_DB;          // uncalibrated

    Serial.println("┌─ INMP441 microphone ────────────────────────────");
    Serial.printf( "│  RMS = %.1f dBFS  (SPL est. ≈ %.1f dB, uncalibrated)%s\n",
                   dBFS, splEst, clipping ? "  *** CLIP ***" : "");
    Serial.println("│  Calibrate offset: compare to a ref SPL meter.");
    Serial.println("│  Noise floor ≈ 33 dB(A) — can't read below that.");
    Serial.println("└──────────────────────────────────────────────────");
}

// ---------------- Setup ----------------

static void ledBlink(int times, int on_ms = 120, int off_ms = 120) {
    for (int i = 0; i < times; i++) {
        digitalWrite(PIN_LED, HIGH);
        delay(on_ms);
        digitalWrite(PIN_LED, LOW);
        if (i < times - 1) delay(off_ms);
    }
}

void setup() {
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    Serial.begin(115200);
    delay(1000);

    // 3 quick blinks = boot started
    ledBlink(3, 80, 80);

    Serial.println();
    Serial.println("================================");
    Serial.println(" ESP32 Sensor Test");
    Serial.println("================================");

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    Wire.setPins(I2C_SDA, I2C_SCL);
    Wire.begin();

    // WiFi + OTA
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("WiFi connected — ");
        Serial.println(WiFi.localIP());
        ArduinoOTA.setHostname("esp32-sensor-test");
        ArduinoOTA.onStart([]()  { Serial.println("OTA start"); });
        ArduinoOTA.onEnd([]()    { Serial.println("OTA done"); });
        ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
            Serial.printf("OTA %u%%\r", p * 100 / t);
        });
        ArduinoOTA.begin();
        Serial.println("OTA ready");
    } else {
        Serial.println("WiFi failed — OTA disabled");
    }

    initSensors();

    // 1 long blink = setup complete
    ledBlink(1, 600);
}

// ---------------- Loop ----------------

void loop() {
    ArduinoOTA.handle();

    Serial.println("\n=================== sensor read ===================");

    if (hasSen66)  readSen66();
    if (hasAdxl)   readAdxl();
    if (hasBh1750) readBh1750();
    readGas();
    readSoil();
    readBattery();
    if (hasMic)    readMic();

    Serial.println("====================================================");

    // Keep OTA responsive during the wait (2 s between full reads).
    for (int i = 0; i < 200; i++) {
        ArduinoOTA.handle();
        delay(10);
    }
}
