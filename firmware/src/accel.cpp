// ADXL345 ground-vibration analysis. See include/accel.h and docs/SENSORS.md.
//
// Pipeline:
//   1. Capture ACCEL_FFT_SAMPLES (512) tri-axis samples; measure actual Fs from timing.
//   2. Per-axis: remove mean (DC/gravity), numerically integrate → velocity, find peak |v|.
//   3. On scalar magnitude |a|-DC: Hann-windowed FFT → 1/3-oct band power and peak freq.
//   4. Legacy RMS/peak from the same AC magnitude vector.

#include <Arduino.h>
#include <math.h>
#include <arduinoFFT.h>
#include "config.h"
#include "accel.h"

// 1/3-oct band centres (Hz): 4, 8, 16, 31.5, 63, 125
const float ACCEL_BAND_CENTERS[ACCEL_NBANDS] = {4.0f, 8.0f, 16.0f, 31.5f, 63.0f, 125.0f};

// 1/3-octave edges: lower = fc × 2^(-1/6), upper = fc × 2^(+1/6).
// Pre-computed constants (upper edge of band b == lower edge of band b+1):
//   4 Hz  → [3.56, 4.49]
//   8 Hz  → [7.13, 8.98]
//  16 Hz  → [14.3, 18.0]
//  31.5 Hz → [28.1, 35.4]
//  63 Hz  → [56.1, 70.7]
// 125 Hz  → [111,  140]
static const float BAND_LOW[ACCEL_NBANDS]  = {3.56f, 7.13f, 14.3f, 28.1f, 56.1f, 111.0f};
static const float BAND_HIGH[ACCEL_NBANDS] = {4.49f, 8.98f, 18.0f, 35.4f, 70.7f, 140.0f};

// Working buffers (static — kept off the stack; reused between calls).
static float s_x[ACCEL_FFT_SAMPLES];
static float s_y[ACCEL_FFT_SAMPLES];
static float s_z[ACCEL_FFT_SAMPLES];
static float s_vReal[ACCEL_FFT_SAMPLES];
static float s_vImag[ACCEL_FFT_SAMPLES];

static ArduinoFFT<float> s_fft =
    ArduinoFFT<float>(s_vReal, s_vImag, ACCEL_FFT_SAMPLES, 1.0f /* sampleRate set per call */);

// Numerically integrate acc[] (m/s²) at sample rate fs_hz using trapezoidal rule,
// returning peak |velocity| in m/s.  Mean is removed first to eliminate DC/gravity.
static float peakVelocity(const float* acc, int n, float fs_hz) {
    double mean = 0.0;
    for (int i = 0; i < n; ++i) mean += acc[i];
    mean /= n;

    const float dt = 1.0f / fs_hz;
    double v = 0.0;
    float  peak = 0.0f;
    for (int i = 1; i < n; ++i) {
        // trapezoidal: v += ((a[i]-mean) + (a[i-1]-mean)) / 2 * dt
        v += 0.5 * ((acc[i] - mean) + (acc[i - 1] - mean)) * dt;
        const float av = fabsf((float)v);
        if (av > peak) peak = av;
    }
    return peak;
}

bool accel_capture(Adafruit_ADXL345_Unified& dev, AccelResult& out) {
    sensors_event_t e;

    // ---- 1. Capture samples, measure actual Fs ----
    const uint32_t t0 = micros();
    for (int i = 0; i < ACCEL_FFT_SAMPLES; ++i) {
        dev.getEvent(&e);
        s_x[i] = e.acceleration.x;
        s_y[i] = e.acceleration.y;
        s_z[i] = e.acceleration.z;
        if (ACCEL_FFT_GAP_US > 0) delayMicroseconds(ACCEL_FFT_GAP_US);
    }
    const uint32_t elapsed_us = micros() - t0;
    const float fs = (float)ACCEL_FFT_SAMPLES / (elapsed_us * 1e-6f);

    // ---- 2. Legacy broadband RMS / peak on AC vector magnitude ----
    double sumMag = 0.0;
    for (int i = 0; i < ACCEL_FFT_SAMPLES; ++i) {
        const float m = sqrtf(s_x[i]*s_x[i] + s_y[i]*s_y[i] + s_z[i]*s_z[i]);
        s_vReal[i] = m;
        sumMag += m;
    }
    const float meanMag = (float)(sumMag / ACCEL_FFT_SAMPLES);
    out.mag_mean = meanMag;
    out.x = s_x[ACCEL_FFT_SAMPLES - 1];
    out.y = s_y[ACCEL_FFT_SAMPLES - 1];
    out.z = s_z[ACCEL_FFT_SAMPLES - 1];

    double sumsq = 0.0;
    float  peak  = 0.0f;
    for (int i = 0; i < ACCEL_FFT_SAMPLES; ++i) {
        const float ac = s_vReal[i] - meanMag;
        sumsq += (double)ac * ac;
        const float a = fabsf(ac);
        if (a > peak) peak = a;
    }
    out.rumble_rms  = sqrtf((float)(sumsq / ACCEL_FFT_SAMPLES));
    out.rumble_peak = peak;

    // ---- 3. PPV via per-axis integration ----
    const float ppvX = peakVelocity(s_x, ACCEL_FFT_SAMPLES, fs);
    const float ppvY = peakVelocity(s_y, ACCEL_FFT_SAMPLES, fs);
    const float ppvZ = peakVelocity(s_z, ACCEL_FFT_SAMPLES, fs);
    // PPV = max component (ISO 4866 / SBR-A convention)
    out.ppv_m_s = fmaxf(ppvX, fmaxf(ppvY, ppvZ));

    // ---- 4. FFT on AC vector magnitude → 1/3-oct bands + dominant frequency ----
    // Reload s_vReal with AC magnitude (gravity removed), clear imag part.
    double meanAC = 0.0;
    for (int i = 0; i < ACCEL_FFT_SAMPLES; ++i) {
        s_vReal[i] = s_vReal[i] - meanMag;   // already computed above
        s_vImag[i] = 0.0f;
        meanAC += s_vReal[i];
    }
    // Note: meanAC should be ~0 since we subtracted the mean, but re-centre to be safe.
    meanAC /= ACCEL_FFT_SAMPLES;
    for (int i = 0; i < ACCEL_FFT_SAMPLES; ++i) s_vReal[i] -= (float)meanAC;

    // Update ArduinoFFT with actual sample rate (constructor stored a copy).
    s_fft = ArduinoFFT<float>(s_vReal, s_vImag, ACCEL_FFT_SAMPLES, fs);
    s_fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
    s_fft.compute(FFTDirection::Forward);
    s_fft.complexToMagnitude();  // result in s_vReal[0..N/2]

    const float binHz = fs / (float)ACCEL_FFT_SAMPLES;

    // Dominant frequency: highest magnitude bin above 1 Hz (skip DC and sub-Hz).
    float maxMag  = 0.0f;
    int   peakBin = 0;
    for (int i = 1; i < ACCEL_FFT_SAMPLES / 2; ++i) {
        if (i * binHz < 1.0f) continue;
        if (s_vReal[i] > maxMag) { maxMag = s_vReal[i]; peakBin = i; }
    }
    const float peakHz = peakBin * binHz;
    out.dom_freq_hz = (peakHz >= 1.0f && peakHz <= 255.0f) ? (uint8_t)lroundf(peakHz) : 0;

    // 1/3-octave band power (sum of squared FFT magnitudes in band / N²)
    // Reference: 1 m/s² → 0 dBm/s²
    const float norm2 = (float)ACCEL_FFT_SAMPLES * (float)ACCEL_FFT_SAMPLES;
    for (int b = 0; b < ACCEL_NBANDS; ++b) {
        double bandPow = 0.0;
        for (int i = 1; i < ACCEL_FFT_SAMPLES / 2; ++i) {
            const float f = i * binHz;
            if (f < BAND_LOW[b] || f >= BAND_HIGH[b]) continue;
            // FFT magnitude after complexToMagnitude is sum of +/- frequency contribution;
            // dividing by N gives RMS amplitude; half-spectrum so ×2 for two-sided energy.
            const float amp = s_vReal[i] / (float)ACCEL_FFT_SAMPLES;
            bandPow += (double)amp * amp * 2.0;
        }
        // dBm/s² = 10 × log10(mean_sq / ref²) where ref = 1 m/s²
        out.band_db[b] = (bandPow > 0.0) ? 10.0f * log10f((float)bandPow) : -120.0f;
    }

    return true;
}
