#pragma once
// INMP441 I2S microphone — relative sound-pressure analysis.
// Captures multiple overlapping windows per call (~1.3 s total), computes:
//   - LAeq  : A-weighted equivalent level over the full capture
//   - LAmax : max A-weighted level across the individual windows (Dutch aircraft metric)
//   - LCeq  : C-weighted equivalent level (LCeq − LAeq > 6 dB flags low-frequency noise)
//   - per-octave-band levels (unweighted + A-weighted)
// See docs/SENSORS.md for calibration math.

#include <stdint.h>

// Standard octave-band centre frequencies (Hz).
static constexpr int MIC_NBANDS = 9;
extern const float MIC_BAND_CENTERS[MIC_NBANDS];   // 31.5 .. 8000

struct MicResult {
    float rms_dbfs;                  // broadband RMS, dB re full scale (<=0)
    float spl_est;                   // uncalibrated SPL estimate, dB
    float laeq_est;                  // A-weighted LAeq over full capture window, dB(A)
    float lamax_dba;                 // peak per-window LAeq ≈ LAmax, dB(A)
    float lceq;                      // C-weighted LCeq over full capture window, dB(C)
    float band_db[MIC_NBANDS];       // per-band level, unweighted, dB SPL est.
    float band_dba[MIC_NBANDS];      // per-band level, A-weighted, dB(A) est.
    bool  clipping;                  // any sample at/over the acoustic overload point
};

// Initialise the I2S peripheral. Returns false on driver error.
bool mic_begin();

// Change the I2S sample rate at runtime (POWER_SAVING lowers it to cut FFT CPU).
// No-op if unchanged; returns false on driver error.
bool mic_set_rate(uint32_t hz);

// Capture ~1.3 s of audio (30 FFT windows) and fill `out`. Returns false on read error.
bool mic_capture(MicResult& out);

// Boot self-test: capture one window and confirm live, non-stuck data.
bool mic_selftest(float& out_dbfs);
