// Air Monitor — ESP32-D0WDQ6 sensor node.
// SEN66-only mode for initial sensor testing.

#include <Arduino.h>
#include <Wire.h>
#include <SensirionI2cSen66.h>
#include "config.h"

static SensirionI2cSen66 sen66;

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

    delay(10);
}
