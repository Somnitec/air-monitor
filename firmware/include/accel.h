#pragma once
// ADXL345 ground-vibration analysis — see docs/SENSORS.md.
//
// Each call to accel_capture() collects ACCEL_FFT_SAMPLES (512) samples from
// all three axes, then computes:
//   - 1/3-octave spectral levels in dB re 1 m/s² (the infrasound / vibration range)
//   - Peak Particle Velocity (PPV) via frequency-domain integration per axis
//   - Dominant frequency (FFT peak on the vector magnitude spectrum)
//   - Legacy broadband RMS/peak (retained for backward compat with record.h v1 fields)
//
// Band centres (ACCEL_NBANDS = 6):  4, 8, 16, 31.5, 63, 125 Hz
// (Relevant for aircraft-induced ground vibration and infrasound — Dutch SBR-A / ISO 4866)

#include <Adafruit_ADXL345_U.h>
#include "config.h"   // ACCEL_NBANDS

struct AccelResult {
    // Legacy broadband metrics (retained for the v1 wire fields)
    float rumble_rms;   // RMS of AC vector magnitude over window, m/s²
    float rumble_peak;  // peak AC vector magnitude, m/s²
    float mag_mean;     // mean |a| ≈ 9.81 m/s² when stationary (gravity)
    float x, y, z;     // last instantaneous sample, m/s² (orientation reference)

    // v2 — Dutch/EU infrasound & vibration metrics
    float   ppv_m_s;                    // Peak Particle Velocity, m/s (max across X/Y/Z)
    uint8_t dom_freq_hz;                // dominant vibration frequency, Hz (0 = no signal)
    float   band_db[ACCEL_NBANDS];      // 1/3-oct levels: 4,8,16,31.5,63,125 Hz in dBm/s²
};

// Band centres (Hz) for the ACCEL_NBANDS 1/3-octave bands, matching accel_bands[] order.
extern const float ACCEL_BAND_CENTERS[ACCEL_NBANDS];

// Capture one vibration window from an already-initialised ADXL345.
// Always fills `out`; returns false only if the device pointer is unusable.
bool accel_capture(Adafruit_ADXL345_Unified& dev, AccelResult& out);
