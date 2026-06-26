// ADXL345 ground-rumble analysis. See include/accel.h and docs/SENSORS.md.
//
// Pipeline (mirrors mic.cpp at lower fidelity):
//   1. read ACCEL_RUMBLE_SAMPLES of |a| as fast as I2C allows (~ACCEL_RUMBLE_GAP_US apart)
//   2. compute mean |a| (≈ gravity) and remove it -> AC component
//   3. report RMS of the AC component (rumble level) and its peak excursion
//
// We work on the vector magnitude |a| = sqrt(x^2+y^2+z^2) so the result is
// orientation-independent: it doesn't matter how the station is mounted.

#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "accel.h"

bool accel_capture(Adafruit_ADXL345_Unified& dev, AccelResult& out) {
    static float mag[ACCEL_RUMBLE_SAMPLES];
    sensors_event_t e;

    float lastX = 0, lastY = 0, lastZ = 0;
    double sum = 0.0;

    for (int i = 0; i < ACCEL_RUMBLE_SAMPLES; ++i) {
        dev.getEvent(&e);
        lastX = e.acceleration.x;
        lastY = e.acceleration.y;
        lastZ = e.acceleration.z;
        const float m = sqrtf(lastX * lastX + lastY * lastY + lastZ * lastZ);
        mag[i] = m;
        sum += m;
        if (ACCEL_RUMBLE_GAP_US > 0) delayMicroseconds(ACCEL_RUMBLE_GAP_US);
    }

    const float mean = (float)(sum / ACCEL_RUMBLE_SAMPLES);

    double sumsq = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < ACCEL_RUMBLE_SAMPLES; ++i) {
        const float ac = mag[i] - mean;     // gravity removed
        sumsq += (double)ac * ac;
        const float a = fabsf(ac);
        if (a > peak) peak = a;
    }

    out.rumble_rms  = sqrtf((float)(sumsq / ACCEL_RUMBLE_SAMPLES));
    out.rumble_peak = peak;
    out.mag_mean    = mean;
    out.x = lastX;
    out.y = lastY;
    out.z = lastZ;
    return true;
}
