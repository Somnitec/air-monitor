// INMP441 I2S microphone analysis — see mic.h and docs/SENSORS.md.
//
// Each call to mic_capture() runs MIC_WINDOWS consecutive FFT windows (~1.3 s
// total at 48 kHz / 2048 samples per window).  Per window:
//   1. Read MIC_FFT_SAMPLES 32-bit I2S slots (24-bit sample in upper bits).
//   2. Remove DC, compute RMS → dBFS and A-weighted + C-weighted levels.
//   3. Hann-window + FFT, accumulate octave-band power (unweighted + A-weighted).
// Across all windows:
//   - LAeq  = energy-average of per-window A-weighted levels.
//   - LAmax = maximum per-window level (Dutch/EU aircraft noise metric).
//   - LCeq  = energy-average of per-window C-weighted levels.

#include <Arduino.h>
#include <math.h>
#include "driver/i2s.h"
#include <arduinoFFT.h>
#include "config.h"
#include "mic.h"

// Number of consecutive FFT windows per mic_capture() call.
// 30 × (2048/48000 s) ≈ 1.28 s total — enough to resolve LAmax for slow events.
static constexpr int MIC_WINDOWS = 30;

const float MIC_BAND_CENTERS[MIC_NBANDS] =
    {31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const float FULL_SCALE = 8388608.0f;   // 2^23, magnitude of full-scale 24-bit

// Active sample rate. Defaults to the compile-time rate; POWER_SAVING lowers it at
// runtime via mic_set_rate() to halve FFT CPU. The band/bin math reads this, not the
// macro, so a rate change takes effect on the next capture.
static uint32_t s_sampleRate = I2S_SAMPLE_RATE_HZ;

// Working buffers (static: live off the stack, reused across windows).
static int32_t  s_raw[MIC_FFT_SAMPLES];
static float    s_vReal[MIC_FFT_SAMPLES];
static float    s_vImag[MIC_FFT_SAMPLES];

static ArduinoFFT<float> s_fft =
    ArduinoFFT<float>(s_vReal, s_vImag, MIC_FFT_SAMPLES, (float)I2S_SAMPLE_RATE_HZ);

// IEC 61672 A-weighting — linear gain (1.0 at 1 kHz).
static float aWeightGain(float f) {
    const float f2 = f * f;
    const float num = 12194.0f * 12194.0f * f2 * f2;
    const float den = (f2 + 20.6f * 20.6f)
                    * sqrtf((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f))
                    * (f2 + 12194.0f * 12194.0f);
    const float ra = num / den;
    // +2.00 dB normalisation so A(1kHz) = 0 dB
    return powf(10.0f, (20.0f * log10f(ra) + 2.0f) / 20.0f);
}

// IEC 61672 C-weighting — linear gain (1.0 at 1 kHz).
// C(f) = 12194² × f² / ((f²+20.6²)(f²+12194²)) ; +0.06 dB normalisation.
static float cWeightGain(float f) {
    const float f2 = f * f;
    const float num = 12194.0f * 12194.0f * f2;
    const float den = (f2 + 20.6f * 20.6f) * (f2 + 12194.0f * 12194.0f);
    const float rc  = num / den;
    return powf(10.0f, (20.0f * log10f(rc) + 0.06f) / 20.0f);
}

// Whether the I2S driver is currently installed, so mic_begin()/mic_end() are
// idempotent and mic_reinit() can safely tear down before re-installing.
static bool s_installed = false;

bool mic_begin() {
    if (s_installed) i2s_driver_uninstall(I2S_PORT);   // idempotent re-begin
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
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) { i2s_driver_uninstall(I2S_PORT); return false; }
    s_installed  = true;
    s_sampleRate = I2S_SAMPLE_RATE_HZ;
    return true;
}

void mic_end() {
    if (!s_installed) return;
    i2s_driver_uninstall(I2S_PORT);
    s_installed = false;
}

bool mic_reinit(uint32_t hz) {
    mic_end();
    if (!mic_begin()) return false;
    return mic_set_rate(hz);   // no-op when hz == I2S_SAMPLE_RATE_HZ
}

// Change the I2S clock at runtime (POWER_SAVING uses a lower rate to cut FFT CPU).
// No-op if the rate is unchanged. Returns false on driver error (rate left as-was).
bool mic_set_rate(uint32_t hz) {
    if (hz == s_sampleRate) return true;
    // 32-bit slots, single (mono) channel — must match mic_begin()'s i2s_config_t.
    if (i2s_set_clk(I2S_PORT, hz, I2S_BITS_PER_SAMPLE_32BIT, I2S_CHANNEL_MONO) != ESP_OK)
        return false;
    s_sampleRate = hz;
    return true;
}

// Read one full window of samples into s_raw. Returns false on short/failed read.
static bool readWindow() {
    size_t bytesRead = 0;
    const esp_err_t r = i2s_read(I2S_PORT, (void*)s_raw, sizeof(s_raw), &bytesRead,
                                 pdMS_TO_TICKS(1000));
    return (r == ESP_OK && bytesRead == sizeof(s_raw));
}

// Process the current s_raw window; accumulate into caller-provided totals.
// Returns per-window A-weighted level in dB(A) (for LAmax tracking).
static float processWindow(double& totPowerA, double& totPowerC, double& totPowerRaw,
                           double bandPA[MIC_NBANDS], double bandPRaw[MIC_NBANDS],
                           bool& clipping) {
    // Convert raw I2S → float, remove DC
    double mean = 0.0;
    for (int i = 0; i < MIC_FFT_SAMPLES; ++i) {
        s_vReal[i] = (float)(s_raw[i] >> 8);   // 24-bit sample in upper bits
        mean += s_vReal[i];
    }
    mean /= MIC_FFT_SAMPLES;

    double sumsqA = 0.0, sumsqC = 0.0, sumsqRaw = 0.0;
    const float clip = FULL_SCALE * 0.999f;
    for (int i = 0; i < MIC_FFT_SAMPLES; ++i) {
        s_vReal[i] -= (float)mean;
        s_vImag[i]  = 0.0f;
        if (fabsf(s_vReal[i]) >= clip) clipping = true;
        sumsqRaw += (double)s_vReal[i] * s_vReal[i];
    }

    // FFT for band analysis
    s_fft.windowing(FFTWindow::Hann, FFTDirection::Forward);
    s_fft.compute(FFTDirection::Forward);
    s_fft.complexToMagnitude();

    const float binHz = (float)s_sampleRate / (float)MIC_FFT_SAMPLES;
    for (int i = 1; i < MIC_FFT_SAMPLES / 2; ++i) {
        const float f = i * binHz;
        if (f < 22.0f) continue;
        const float mag = s_vReal[i];
        const float p   = mag * mag;
        const float aG  = aWeightGain(f);
        const float cG  = cWeightGain(f);
        const float pa  = p * aG * aG;
        const float pc  = p * cG * cG;
        sumsqA  += pa;
        sumsqC  += pc;
        totPowerA   += pa;
        totPowerC   += pc;
        totPowerRaw += p;

        const int b = (int)lroundf(log2f(f / 31.5f));
        if (b >= 0 && b < MIC_NBANDS) {
            bandPA[b]   += pa;
            bandPRaw[b] += p;
        }
    }
    totPowerRaw += sumsqRaw;  // also accumulate time-domain RMS for dBFS

    // Per-window A-weighted level (for LAmax)
    const double ref = (double)FULL_SCALE * FULL_SCALE * (MIC_FFT_SAMPLES / 2.0);
    return (sumsqA > 0) ? 10.0f * log10f((float)(sumsqA / ref)) + MIC_DBFS_TO_SPL_DB : -120.0f;
}

bool mic_capture(MicResult& out) {
    double totalPowerA   = 0.0;
    double totalPowerC   = 0.0;
    double totalPowerRaw = 0.0;
    double bandPA[MIC_NBANDS]   = {0};
    double bandPRaw[MIC_NBANDS] = {0};
    float  maxWindowLA = -120.0f;
    bool   anyClip     = false;
    int    valid       = 0;

    for (int w = 0; w < MIC_WINDOWS; ++w) {
        if (!readWindow()) continue;
        float windowLA = processWindow(totalPowerA, totalPowerC, totalPowerRaw,
                                       bandPA, bandPRaw, anyClip);
        if (windowLA > maxWindowLA) maxWindowLA = windowLA;
        ++valid;
    }
    if (valid == 0) return false;

    const double ref = (double)FULL_SCALE * FULL_SCALE * (MIC_FFT_SAMPLES / 2.0);

    // Broadband RMS (time-domain) across all windows
    double rmsRaw = sqrtf((float)(totalPowerRaw / ((double)valid * MIC_FFT_SAMPLES)));
    out.rms_dbfs = (rmsRaw > 0) ? 20.0f * log10f((float)(rmsRaw / FULL_SCALE)) : -120.0f;
    out.spl_est  = out.rms_dbfs + MIC_DBFS_TO_SPL_DB;

    // Energy-averaged A-weighted and C-weighted levels
    const double refW = ref * valid;
    out.laeq_est  = (totalPowerA > 0) ? 10.0f * log10f((float)(totalPowerA / refW)) + MIC_DBFS_TO_SPL_DB : -120.0f;
    out.lceq      = (totalPowerC > 0) ? 10.0f * log10f((float)(totalPowerC / refW)) + MIC_DBFS_TO_SPL_DB : -120.0f;
    out.lamax_dba = maxWindowLA;
    out.clipping  = anyClip;

    for (int b = 0; b < MIC_NBANDS; ++b) {
        out.band_db[b]  = (bandPRaw[b] > 0) ? 10.0f * log10f((float)(bandPRaw[b] / refW)) + MIC_DBFS_TO_SPL_DB : -120.0f;
        out.band_dba[b] = (bandPA[b]   > 0) ? 10.0f * log10f((float)(bandPA[b]   / refW)) + MIC_DBFS_TO_SPL_DB : -120.0f;
    }
    return true;
}

bool mic_selftest(float& out_dbfs) {
    if (!readWindow()) return false;
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
