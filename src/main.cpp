// Air Monitor — ESP32-D0WDQ6 sensor node.
// Boot sequence: bring up each bus, then probe every sensor one-by-one and print
// a PASS/FAIL table, then stream readings. See docs/SENSORS.md for interpretation.
//
// Sensors: SEN66 (I2C), ADXL345 (I2C), BH1750/GY-30 (I2C),
//          capacitive soil + SEN0564 CO + SEN0563 HCHO (analog ADC1),
//          INMP441 (I2S microphone, relative sound pressure).

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cSen66.h>
#include <Adafruit_ADXL345_U.h>
#include <BH1750.h>
#include "config.h"
#include "mic.h"

static SensirionI2cSen66 sen66;
static Adafruit_ADXL345_Unified adxl(12345);
static BH1750 bh1750(ADDR_BH1750);

// Track which sensors came up so loop() only reads what's present.
static struct {
    bool sen66, adxl, bh1750, soil, co, hcho, mic;
} present;

// ---- small helpers --------------------------------------------------------

static bool i2cAck(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

static uint8_t i2cReadReg(uint8_t addr, uint8_t reg) {
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom((int)addr, 1);
    return Wire.available() ? Wire.read() : 0;
}

static uint32_t readAdcMv(int pin) {
    uint32_t acc = 0;
    for (int i = 0; i < GAS_ADC_OVERSAMPLE; ++i) acc += analogReadMilliVolts(pin);
    return acc / GAS_ADC_OVERSAMPLE;
}

// MEMS MOS sensor resistance from the divider (see docs/SENSORS.md).
static float gasRs(float vout_mv) {
    if (vout_mv < 1.0f) return INFINITY;
    return GAS_LOAD_R_OHMS * (GAS_VCC_MV - vout_mv) / vout_mv;
}

static void printResult(const char* name, bool ok, const char* detail) {
    Serial.printf("  [%s] %-10s  %s\n", ok ? "PASS" : "FAIL", name, detail);
}

// ---- per-sensor probes ----------------------------------------------------

static void probeI2cSensors() {
    char buf[64];

    // SEN66: ACK at 0x6B, then try to start a measurement.
    if (i2cAck(ADDR_SEN66)) {
        sen66.begin(Wire, ADDR_SEN66);
        int16_t r = sen66.startContinuousMeasurement();
        present.sen66 = (r == 0);
        snprintf(buf, sizeof(buf), present.sen66 ? "0x6B, measurement started"
                                                 : "0x6B ACK but start err=%d", r);
        printResult("SEN66", present.sen66, buf);
    } else {
        printResult("SEN66", false, "no ACK at 0x6B");
    }

    // ADXL345: ACK at 0x53, confirm DEVID, then library begin().
    if (i2cAck(ADDR_ADXL345)) {
        uint8_t devid = i2cReadReg(ADDR_ADXL345, 0x00);
        if (devid == ADXL345_DEVID && adxl.begin(ADDR_ADXL345)) {
            adxl.setRange(ADXL345_RANGE_4_G);
            present.adxl = true;
            printResult("ADXL345", true, "0x53, DEVID 0xE5 ok");
        } else {
            snprintf(buf, sizeof(buf), "0x53 ACK but DEVID=0x%02X (want 0xE5)", devid);
            printResult("ADXL345", false, buf);
        }
    } else {
        printResult("ADXL345", false, "no ACK at 0x53");
    }

    // BH1750 / GY-30.
    if (i2cAck(ADDR_BH1750)) {
        present.bh1750 = bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, ADDR_BH1750);
        printResult("BH1750", present.bh1750,
                    present.bh1750 ? "0x23, hi-res mode" : "0x23 ACK but begin() failed");
    } else {
        printResult("BH1750", false, "no ACK at 0x23");
    }
}

// Analog sensors can't truly be "detected" — flag plausible vs floating/rail.
static void probeAnalog(const char* name, int pin, bool& flag) {
    uint32_t mv = readAdcMv(pin);
    flag = (mv >= ANALOG_PRESENT_MIN_MV && mv <= ANALOG_PRESENT_MAX_MV);
    char buf[64];
    snprintf(buf, sizeof(buf), "%u mV %s", mv,
             flag ? "(plausible)" : "(floating/at-rail? check wiring)");
    printResult(name, flag, buf);
}

static void selfTest() {
    Serial.println("\n[selftest] probing sensors one-by-one...");

    Serial.println(" I2C bus (SDA=16 SCL=17):");
    probeI2cSensors();

    Serial.println(" Analog (ADC1):");
    probeAnalog("SOIL", PIN_SOIL_ADC, present.soil);
    probeAnalog("CO(0564)", PIN_GAS_CO_ADC, present.co);
    probeAnalog("HCHO(0563)", PIN_GAS_HCHO_ADC, present.hcho);

    Serial.println(" I2S mic (INMP441, BCLK=26 WS=25 SD=33):");
    float dbfs = 0;
    if (mic_begin() && mic_selftest(dbfs)) {
        present.mic = true;
        char buf[64];
        snprintf(buf, sizeof(buf), "live, RMS %.1f dBFS", dbfs);
        printResult("INMP441", true, buf);
    } else {
        printResult("INMP441", false, "no/stuck I2S data — check wiring & L/R->GND");
    }

    int ok = present.sen66 + present.adxl + present.bh1750 +
             present.soil + present.co + present.hcho + present.mic;
    Serial.printf("[selftest] %d/7 sensors responding.\n\n", ok);
}

// ---- setup / loop ---------------------------------------------------------

void setup() {
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.begin(115200);
    delay(300);
    Serial.println("\n[boot] air-monitor");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_SOIL_ADC,     ADC_11db);
    analogSetPinAttenuation(PIN_GAS_CO_ADC,   ADC_11db);
    analogSetPinAttenuation(PIN_GAS_HCHO_ADC, ADC_11db);

    selfTest();

    digitalWrite(PIN_STATUS_LED, HIGH);   // solid = boot done
}

void loop() {
    static uint32_t tSen = 0, tGas = 0, tEnv = 0, tMic = 0;
    const uint32_t now = millis();

    if (present.sen66 && now - tSen >= SAMPLE_MS_SEN66) {
        tSen = now;
        float pm1, pm25, pm4, pm10, t, rh, voc, nox; uint16_t co2;
        if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, t, rh, voc, nox, co2) == 0) {
            Serial.printf("SEN66: PM2.5=%.1f PM10=%.1f ug/m3  VOCidx=%.0f NOxidx=%.0f "
                          "CO2=%u ppm  T=%.1fC RH=%.0f%%\n",
                          pm25, pm10, voc, nox, co2, t, rh);
        }
    }

    if ((present.soil || present.co || present.hcho) && now - tGas >= SAMPLE_MS_GAS) {
        tGas = now;
        if (present.co || present.hcho) {
            uint32_t co_mv   = readAdcMv(PIN_GAS_CO_ADC);
            uint32_t hcho_mv = readAdcMv(PIN_GAS_HCHO_ADC);
            // Rs/R0 needs a stored clean-air R0; here we print Rs so you can capture it.
            Serial.printf("GAS:   CO=%u mV (Rs=%.0f ohm)  HCHO=%u mV (Rs=%.0f ohm)  [relative]\n",
                          co_mv, gasRs(co_mv), hcho_mv, gasRs(hcho_mv));
        }
        if (present.soil) {
            uint32_t mv = readAdcMv(PIN_SOIL_ADC);
            float pct = 100.0f * (float)(SOIL_DRY_MV - (int)mv) / (float)(SOIL_DRY_MV - SOIL_WET_MV);
            pct = constrain(pct, 0.0f, 100.0f);
            Serial.printf("SOIL:  %u mV -> %.0f%% (calibrate SOIL_DRY/WET_MV)\n", mv, pct);
        }
    }

    if (present.bh1750 && now - tEnv >= SAMPLE_MS_BH1750) {
        tEnv = now;
        if (bh1750.measurementReady()) {
            Serial.printf("LIGHT: %.1f lux\n", bh1750.readLightLevel());
        }
        if (present.adxl) {
            sensors_event_t e; adxl.getEvent(&e);
            float mag = sqrtf(e.acceleration.x * e.acceleration.x +
                              e.acceleration.y * e.acceleration.y +
                              e.acceleration.z * e.acceleration.z);
            Serial.printf("ACCEL: x=%.2f y=%.2f z=%.2f |a|=%.2f m/s2\n",
                          e.acceleration.x, e.acceleration.y, e.acceleration.z, mag);
        }
    }

    if (present.mic && now - tMic >= SAMPLE_MS_MIC_RMS) {
        tMic = now;
        MicResult m;
        if (mic_capture(m)) {
            Serial.printf("NOISE: %.1f dB(A) est  (%.1f dBFS, SPL~%.1f dB)%s\n",
                          m.laeq_est, m.rms_dbfs, m.spl_est, m.clipping ? " CLIP!" : "");
            Serial.print("  bands dB(A): ");
            for (int b = 0; b < MIC_NBANDS; ++b)
                Serial.printf("%g:%.0f ", MIC_BAND_CENTERS[b], m.band_dba[b]);
            Serial.println();
        }
    }

    delay(5);
}
