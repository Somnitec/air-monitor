#pragma once
// ADXL345 ground-rumble analysis — the accelerometer analogue of the mic's level
// metering, but coarser. We capture a short burst of |a| samples, remove gravity
// (the DC component), and report the AC magnitude: an RMS "rumble level" plus the
// peak excursion. This is what catches passing trucks, aircraft ground rumble, a
// washing machine spin cycle, footsteps, etc., without storing a waveform.
//
// Units are m/s^2 of *AC* acceleration (gravity removed), so a perfectly still
// sensor reads ~0. See docs/SENSORS.md for the ADXL345 scale notes.

#include <Adafruit_ADXL345_U.h>

struct AccelResult {
    float rumble_rms;   // RMS of (|a| - mean|a|) over the window, m/s^2  (the "level")
    float rumble_peak;  // max |(|a| - mean|a|)| in the window, m/s^2     (worst jolt)
    float mag_mean;     // mean |a| over the window, m/s^2 (~9.81 when still = gravity)
    float x, y, z;      // last instantaneous sample, m/s^2 (orientation reference)
};

// Capture one rumble window from an already-begun ADXL345. Returns false only if
// the device pointer is unusable; otherwise always fills `out`.
bool accel_capture(Adafruit_ADXL345_Unified& dev, AccelResult& out);
