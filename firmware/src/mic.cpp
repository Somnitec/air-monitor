// INMP441 I2S microphone analysis — see mic.h and docs/SENSORS.md.
//
// Pipeline per window:
//   1. read MIC_FFT_SAMPLES 32-bit I2S slots (INMP441 packs a 24-bit sample in
//      the upper bits; we shift to a signed sample and treat FULL_SCALE = 2^23).
//   2. remove DC, compute RMS -> dBFS -> uncalibrated SPL (dBFS + 120).
//   3. Hann-window + FFT, bin magnitude^2 into octave bands, and apply the
//      IEC 61672 A-weighting per bin for the dB(A) / LAeq estimate.

#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"
#include <arduinoFFT.h>
#include "config.h"
#include "mic.h"

const float MIC_BAND_CENTERS[MIC_NBANDS] =
    {31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const float FULL_SCALE = 8388608.0f;   // 2^23, magnitude of full-scale 24-bit

// Working buffers (static so they live off the stack).
static int32_t  s_raw[MIC_FFT_SAMPLES];
static float    s_vReal[MIC_FFT_SAMPLES];
static float    s_vImag[MIC_FFT_SAMPLES];

static ArduinoFFT<float> s_fft =
    ArduinoFFT<float>(s_vReal, s_vImag, MIC_FFT_SAMPLES, (float)I2S_SAMPLE_RATE_HZ);

// IEC 61672 A-weighting, returned as a linear magnitude gain (1.0 at 1 kHz).
static float aWeightGain(float f) {
    const float f2 = f * f;
    const float num = 12194.0f * 12194.0f * f2 * f2;
    const float den = (f2 + 20.6f * 20.6f)
                    * sqrtf((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f))
                    * (f2 + 12194.0f * 12194.0f);
    const float ra = num / den;
    // +2.00 dB normalisation so A(1kHz)=0 dB; convert dB offset to linear gain.
    const float dB = 20.0f * log10f(ra) + 2.0f;
    return powf(10.0f, dB / 20.0f);
}

bool mic_begin() {
    const i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = I2S_SAMPLE_RATE_HZ,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT,  // L/R→GND = LEFT slot, ESP32 driver names it RIGHT
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0,
    };
    const i2s_pin_config_t pins = {
        .bck_io_num = PIN_I2S_BCLK,
        .ws_io_num = PIN_I2S_LRCLK,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = PIN_I2S_DIN,
    };
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;
    return true;
}

// Read one full window of samples into s_raw. Returns false on short/failed read.
static bool readWindow() {
    size_t bytesRead = 0;
    const size_t want = sizeof(s_raw);
    const esp_err_t r = i2s_read(I2S_PORT, (void*)s_raw, want, &bytesRead,
                                 pdMS_TO_TICKS(1000));
    return (r == ESP_OK && bytesRead == want);
}

bool mic_capture(MicResult& out) {
    if (!readWindow()) return false;

    // --- Convert to signed samples, remove DC, accumulate RMS ---
    double mean = 0.0;
    for (int i = 0; i < MIC_FFT_SAMPLES; ++i) {
        // INMP441 sample sits in the top 24 bits of the 32-bit slot.
        const float s = (float)(s_raw[i] >> 8);
        s_vReal[i] = s;
        s_vImag[i] = 0.0f;
        mean += s;
    }
    mean /= MIC_FFT_SAMPLES;

    double sumsq = 0.0;
    bool clipping = false;
    const float clip = FULL_SCALE * 0.999f;   // ~AOP, full-scale 24-bit
    for (int i = 0; i < MIC_FFT_SAMPLES; ++i) {
        s_vReal[i] -= (float)mean;
        sumsq += (double)s_vReal[i] * s_vReal[i];
        if (fabsf(s_vReal[i]) >= clip) clipping = true;
    }
    const float rms = sqrtf((float)(sumsq / MIC_FFT_SAMPLES));
    out.clipping = clipping;
    out.rms_dbfs = (rms > 0.0f) ? 20.0f * log10f(rms / FULL_SCALE) : -120.0f;
    out.spl_est  = out.rms_dbfs + MIC_DBFS_TO_SPL_DB;

    // --- FFT for band analysis (DC already removed; Hann reduces leakage) ---
    s_fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
    s_fft.compute(FFTDirection::Forward);
    s_fft.complexToMagnitude();   // magnitudes now in s_vReal[0..N/2]

    // Accumulate band power (unweighted) and A-weighted band power.
    double bandP[MIC_NBANDS]  = {0};
    double bandPA[MIC_NBANDS] = {0};
    double totalPA = 0.0;
    const float binHz = (float)I2S_SAMPLE_RATE_HZ / (float)MIC_FFT_SAMPLES;

    for (int i = 1; i < MIC_FFT_SAMPLES / 2; ++i) {
        const float f = i * binHz;
        if (f < 22.0f || f > I2S_SAMPLE_RATE_HZ / 2.0f) continue;
        const float mag = s_vReal[i];
        const float p = mag * mag;                       // bin power
        const float aGain = aWeightGain(f);
        const float pa = p * aGain * aGain;              // A-weighted power
        totalPA += pa;

        // octave band = log2(f / 31.5) rounded; bands centred 31.5..8000
        const int b = (int)lroundf(log2f(f / 31.5f));
        if (b >= 0 && b < MIC_NBANDS) {
            bandP[b]  += p;
            bandPA[b] += pa;
        }
    }

    // Convert powers to dB. Reference = FULL_SCALE^2 so 0 dBFS-power maps near the
    // SPL anchor; add the same dBFS->SPL offset to keep bands on the SPL scale.
    const double ref = (double)FULL_SCALE * FULL_SCALE * (MIC_FFT_SAMPLES / 2.0);
    for (int b = 0; b < MIC_NBANDS; ++b) {
        out.band_db[b]  = (bandP[b]  > 0) ? 10.0f * log10f(bandP[b]  / ref) + MIC_DBFS_TO_SPL_DB : -120.0f;
        out.band_dba[b] = (bandPA[b] > 0) ? 10.0f * log10f(bandPA[b] / ref) + MIC_DBFS_TO_SPL_DB : -120.0f;
    }
    out.laeq_est = (totalPA > 0) ? 10.0f * log10f((float)(totalPA / ref)) + MIC_DBFS_TO_SPL_DB : -120.0f;
    return true;
}

bool mic_selftest(float& out_dbfs) {
    if (!readWindow()) return false;
    // A present mic gives varying, non-zero data; a missing one is stuck/zero.
    int32_t first = s_raw[0] >> 8;
    bool varied = false;
    double sumsq = 0.0;
    for (int i = 0; i < MIC_FFT_SAMPLES; ++i) {
        const int32_t s = s_raw[i] >> 8;
        if (s != first) varied = true;
        sumsq += (double)s * s;
    }
    const float rms = sqrtf((float)(sumsq / MIC_FFT_SAMPLES));
    out_dbfs = (rms > 0.0f) ? 20.0f * log10f(rms / FULL_SCALE) : -120.0f;
    return varied && rms > 0.0f;
}
