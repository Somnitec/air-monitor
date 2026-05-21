// Air Monitor — ESP32-D0WDQ6 sensor node.
// SEN66 + analog gas (CO, HCHO) for initial sensor testing.

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cSen66.h>
#include "config.h"

static SensirionI2cSen66 sen66;

// Oversampled millivolt read using the ESP32 ADC eFuse calibration.
static uint32_t readGasMv(int pin) {
    uint32_t acc = 0;
    for (int i = 0; i < GAS_ADC_OVERSAMPLE; ++i) {
        acc += analogReadMilliVolts(pin);
    }
    return acc / GAS_ADC_OVERSAMPLE;
}

static void i2cScan() {
    Serial.println("[i2c] scanning for devices...");
    bool found = false;
    for (uint8_t addr = 1; addr < 127; ++addr) {
        Wire.beginTransmission(addr);
        uint8_t result = Wire.endTransmission();
        if (result == 0) {
            Serial.printf("[i2c] device found at 0x%02X\n", addr);
            found = true;
        }
    }
    if (!found) {
        Serial.println("[i2c] no devices found on bus");
    }
}

void setup() {
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] air-monitor SEN66 test");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);
    i2cScan();

    // ADC1 setup for the analog gas sensors: 12-bit, 0–~3.3 V range.
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_GAS_CO_ADC,   ADC_11db);
    analogSetPinAttenuation(PIN_GAS_HCHO_ADC, ADC_11db);

    sen66.begin(Wire, 0x6B);
    int16_t result = sen66.startContinuousMeasurement();
    if (result != 0) {
        Serial.printf("[sen66] start failed: %d\n", result);
    } else {
        Serial.println("[sen66] continuous measurement started");
    }

    digitalWrite(PIN_STATUS_LED, HIGH);
}

void loop() {
    static uint32_t tSen = 0;
    static uint32_t tGas = 0;
    const uint32_t now = millis();

    static float pm1 = NAN, pm25 = NAN, pm4 = NAN, pm10 = NAN, voc = NAN, nox = NAN, sen_t = NAN, sen_rh = NAN;
    static uint16_t co2 = 0;

    if (now - tSen >= SAMPLE_MS_SEN66) {
        tSen = now;
        if (sen66.readMeasuredValues(pm1, pm25, pm4, pm10, sen_t, sen_rh, voc, nox, co2) != 0) {
            Serial.println("[sen66] read failed");
        } else {
            Serial.printf("SEN66: PM1=%.2f PM2.5=%.2f PM4=%.2f PM10=%.2f VOC=%.2f NOx=%.2f CO2=%u T=%.2f RH=%.2f\n",
                          pm1, pm25, pm4, pm10, voc, nox, co2, sen_t, sen_rh);
        }
    }

    if (now - tGas >= SAMPLE_MS_GAS) {
        tGas = now;
        uint32_t co_mv   = readGasMv(PIN_GAS_CO_ADC);
        uint32_t hcho_mv = readGasMv(PIN_GAS_HCHO_ADC);
        Serial.printf("GAS:   CO=%u mV  HCHO=%u mV (uncalibrated, relative)\n", co_mv, hcho_mv);
    }

    delay(10);
}
