#pragma once
// INMP441 I2S microphone — relative sound-pressure analysis.
// Captures a window, computes broadband RMS (dBFS), an uncalibrated SPL/dB(A)
// estimate, and per-octave-band levels (unweighted + A-weighted) for comparison
// against Dutch noise norms. See docs/SENSORS.md for the interpretation math.

#include <stdint.h>

// Standard octave-band centre frequencies (Hz).
static constexpr int MIC_NBANDS = 9;
extern const float MIC_BAND_CENTERS[MIC_NBANDS];   // 31.5 .. 8000

struct MicResult {
    float rms_dbfs;            // broadband RMS, dB relative to full scale (<=0)
    float spl_est;            // uncalibrated absolute SPL estimate, dB
    float laeq_est;           // uncalibrated A-weighted level, dB(A)
    float band_db[MIC_NBANDS];   // per-band level, unweighted (dB SPL est.)
    float band_dba[MIC_NBANDS];  // per-band level, A-weighted (dB(A) est.)
    bool  clipping;           // any sample at/over the acoustic overload point
};

// Initialise the I2S peripheral. Returns false on driver error.
bool mic_begin();

// Capture one window and fill `out`. Returns false on read error.
bool mic_capture(MicResult& out);

// Boot self-test: capture a window and confirm the mic produces live, non-stuck
// data (a missing/unwired mic reads all-zero or a stuck constant).
bool mic_selftest(float& out_dbfs);
