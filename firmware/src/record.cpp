#include "record.h"
#include "config.h"     // GAS_*_RL_OHMS, GAS_VCC_MV, SOIL_*_MV, BAT_*
#include <ArduinoJson.h>
#include <math.h>
#include <string.h>

// ---- quantization helpers ----
static uint16_t qU16(float v, float scale) {
    if (!(v > 0)) return 0;                  // also catches NaN
    float s = roundf(v * scale);
    if (s > 65535.0f) return 65535;
    return (uint16_t)s;
}
static int16_t qI16(float v, float scale) {
    float s = roundf(v * scale);
    if (s >  32767.0f) return  32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}
static uint16_t qU16c(float v, float scale) {  // clamp-to-zero variant (no negative)
    float s = roundf(v * scale);
    if (s < 0.0f)      return 0;
    if (s > 65535.0f)  return 65535;
    return (uint16_t)s;
}
static uint8_t qU8(float v) {
    float s = roundf(v);
    if (s > 255.0f) return 255;
    if (s < 0.0f)   return 0;
    return (uint8_t)s;
}
static int8_t qI8(float v) {
    float s = roundf(v);
    if (s >  127.0f) return  127;
    if (s < -128.0f) return -128;
    return (int8_t)s;
}
static void setBit(uint16_t& flags, uint16_t bit, bool on) { if (on) flags |= bit; }

// Present bit per group, indexed by RecGroup — mirrors status2 for code that only
// needs "installed?" and keeps the flags word meaningful for old tooling.
static const uint16_t kPresentBit[REC_NGROUPS] = {
    PRESENT_SEN66, PRESENT_BH1750, PRESENT_BME, PRESENT_ADXL,
    PRESENT_CO, PRESENT_HCHO, PRESENT_SOIL, PRESENT_BATTERY, PRESENT_MIC,
};
// A value is stored/usable for a group when it was freshly read or carried forward.
static inline bool groupHasValue(uint8_t st) { return st == FS_OK || st == FS_UNCHANGED; }

Record record_pack(const RecordFields& f) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.seq = f.seq; r.ts = f.ts; r.up_ms = f.up_ms; r.boot = f.boot;

    uint16_t flags = 0;
    setBit(flags, FLAG_TS_OK,      f.ts_ok);
    setBit(flags, FLAG_BAT_CAL,    f.bat_cal);
    setBit(flags, FLAG_NOISE_CLIP, f.noise_clip);

    // Per-group status drives both the 2-bit status2 word and the legacy present bit.
    uint32_t st2 = 0;
    for (int g = 0; g < REC_NGROUPS; ++g) {
        uint8_t s = f.status[g] & 0x3;
        st2 |= (uint32_t)s << (2 * g);
        if (s != FS_ABSENT) flags |= kPresentBit[g];
    }
    r.flags   = flags;
    r.status2 = st2;

    if (groupHasValue(f.status[GRP_SEN66])) {
        r.pm1 = qU16(f.pm1, 10); r.pm25 = qU16(f.pm25, 10);
        r.pm4 = qU16(f.pm4, 10); r.pm10 = qU16(f.pm10, 10);
        r.co2 = f.co2; r.voc = qU16(f.voc, 1); r.nox = qU16(f.nox, 1);
        r.temp = qI16(f.temp, 100); r.rh = qU16(f.rh, 100);
        // v4 — already quantized by the sensor (x10 / ticks); 0xFFFF passes through
        r.pc05 = f.pc05; r.pc1 = f.pc1; r.pc25 = f.pc25; r.pc4 = f.pc4; r.pc10 = f.pc10;
        r.voc_raw = f.voc_raw; r.nox_raw = f.nox_raw;
    }
    if (groupHasValue(f.status[GRP_BH1750])) r.lux = qU16(f.lux, 1);
    if (groupHasValue(f.status[GRP_BME])) {
        r.pressure = qU16(f.pressure, 10);
        r.bme_temp = qI16(f.bme_temp, 100); r.bme_rh = qU16(f.bme_rh, 100);
    }
    if (groupHasValue(f.status[GRP_ADXL])) {
        r.rumble_rms  = qU16(f.rumble_rms,  1000);
        r.rumble_peak = qU16(f.rumble_peak, 1000);
        r.accel_mag   = qU16(f.accel_mag,   100);
        // v2 accel metrics (always packed when adxl present)
        r.ppv_mm10      = qU16c(f.ppv_m_s * 10000.0f, 1.0f);   // m/s → 0.1 mm/s units
        r.accel_dom_hz  = (uint8_t)(f.accel_dom_hz);
        for (int b = 0; b < REC_ACCEL_BANDS; ++b)
            r.accel_bands[b] = qI8(f.accel_band_db[b]);
    }
    if (groupHasValue(f.status[GRP_CO]))      r.co_mv = f.co_mv;
    if (groupHasValue(f.status[GRP_HCHO]))    r.hcho_mv = f.hcho_mv;
    if (groupHasValue(f.status[GRP_SOIL]))    r.soil_mv = f.soil_mv;
    if (groupHasValue(f.status[GRP_BATTERY])) r.bat_raw_mv = f.bat_raw_mv;
    if (groupHasValue(f.status[GRP_MIC])) {
        r.noise_dba  = qI16(f.noise_dba,  10);
        r.noise_spl  = qI16(f.noise_spl,  10);
        r.noise_dbfs = qI16(f.noise_dbfs, 10);
        for (int b = 0; b < REC_NBANDS; ++b) r.bands[b] = qU8(f.bands[b]);
        // v2 mic metrics
        r.noise_lamax = qI16(f.noise_lamax, 10);
        r.noise_lceq  = qI16(f.noise_lceq,  10);
    }
    return r;
}

void record_unpack(const Record& r, RecordFields& f) {
    f.seq = r.seq; f.ts = r.ts; f.up_ms = r.up_ms; f.boot = r.boot;
    f.ts_ok      = r.flags & FLAG_TS_OK;
    f.bat_cal    = r.flags & FLAG_BAT_CAL;
    f.noise_clip = r.flags & FLAG_NOISE_CLIP;

    // Restore per-group status from status2; has_* mirrors "value available".
    for (int g = 0; g < REC_NGROUPS; ++g) f.status[g] = (r.status2 >> (2 * g)) & 0x3;
    f.has_sen66   = groupHasValue(f.status[GRP_SEN66]);
    f.has_bh1750  = groupHasValue(f.status[GRP_BH1750]);
    f.has_bme     = groupHasValue(f.status[GRP_BME]);
    f.has_adxl    = groupHasValue(f.status[GRP_ADXL]);
    f.has_co      = groupHasValue(f.status[GRP_CO]);
    f.has_hcho    = groupHasValue(f.status[GRP_HCHO]);
    f.has_soil    = groupHasValue(f.status[GRP_SOIL]);
    f.has_battery = groupHasValue(f.status[GRP_BATTERY]);
    f.has_mic     = groupHasValue(f.status[GRP_MIC]);

    if (f.has_sen66) {
        f.pm1 = r.pm1/10.0f; f.pm25 = r.pm25/10.0f; f.pm4 = r.pm4/10.0f; f.pm10 = r.pm10/10.0f;
        f.co2 = r.co2; f.voc = r.voc; f.nox = r.nox;
        f.temp = r.temp/100.0f; f.rh = r.rh/100.0f;
        f.pc05 = r.pc05; f.pc1 = r.pc1; f.pc25 = r.pc25; f.pc4 = r.pc4; f.pc10 = r.pc10;
        f.voc_raw = r.voc_raw; f.nox_raw = r.nox_raw;
    }
    if (f.has_bh1750) f.lux = r.lux;
    if (f.has_bme) { f.pressure = r.pressure/10.0f; f.bme_temp = r.bme_temp/100.0f; f.bme_rh = r.bme_rh/100.0f; }
    if (f.has_adxl) {
        f.rumble_rms  = r.rumble_rms  / 1000.0f;
        f.rumble_peak = r.rumble_peak / 1000.0f;
        f.accel_mag   = r.accel_mag   / 100.0f;
        f.ppv_m_s     = r.ppv_mm10    / 10000.0f;   // 0.1 mm/s units → m/s
        f.accel_dom_hz = r.accel_dom_hz;
        for (int b = 0; b < REC_ACCEL_BANDS; ++b) f.accel_band_db[b] = r.accel_bands[b];
    }
    if (f.has_co)      f.co_mv = r.co_mv;
    if (f.has_hcho)    f.hcho_mv = r.hcho_mv;
    if (f.has_soil)    f.soil_mv = r.soil_mv;
    if (f.has_battery) f.bat_raw_mv = r.bat_raw_mv;
    if (f.has_mic) {
        f.noise_dba  = r.noise_dba  / 10.0f;
        f.noise_spl  = r.noise_spl  / 10.0f;
        f.noise_dbfs = r.noise_dbfs / 10.0f;
        for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = r.bands[b];
        f.noise_lamax = r.noise_lamax / 10.0f;
        f.noise_lceq  = r.noise_lceq  / 10.0f;
    }
}

// MEMS MOS resistance from the divider — mirrors firmware.cpp gasRs().
static float gasRsLocal(float vout_mv, float rl_ohms) {
    if (vout_mv < 1.0f) return INFINITY;
    return rl_ohms * (GAS_VCC_MV - vout_mv) / vout_mv;
}

// Emit an explicit JSON null (key present, value null) so the server can tell a
// failed read (FS_INVALID — a real gap) from a carried-forward one (key omitted).
static void jsonNull(JsonDocument& doc, const char* key) { doc[key] = (const char*)nullptr; }

void record_to_json(const Record& r, JsonDocument& doc, bool full_slow) {
    RecordFields f; record_unpack(r, f);

    // A slow group's value is emitted when freshly read (FS_OK) or, for the batch's
    // seed record, when carried forward (FS_UNCHANGED + full_slow) — the value is
    // already in `f` for both. FS_INVALID/FS_ABSENT are unaffected.
    auto emitSlow = [&](uint8_t st) { return st == FS_OK || (full_slow && st == FS_UNCHANGED); };

    doc["ts"]    = f.ts;
    doc["ts_ok"] = f.ts_ok;
    doc["dev"]   = DEVICE_ID;
    doc["up_ms"] = f.up_ms;
    doc["boot"]  = f.boot;
    // Raw per-group status word (2 bits/group, RecGroup order). Without it the server
    // can't tell FS_ABSENT from FS_UNCHANGED — both omit their keys — and forward-fills
    // a dead sensor's last values forever (the overnight-stale-SEN66 incident).
    doc["st2"]   = r.status2;

    // Slow channel: delta-encoded. FS_OK emits values; FS_UNCHANGED & FS_ABSENT omit
    // the keys (the server carries the last value forward / leaves them absent);
    // FS_INVALID emits null to mark a genuine gap. Fast channel (accel/mic) is read
    // every cycle, so it only emits on FS_OK and omits otherwise (no carry-forward).
    if (emitSlow(f.status[GRP_SEN66])) {
        doc["pm1"] = f.pm1; doc["pm25"] = f.pm25; doc["pm4"] = f.pm4; doc["pm10"] = f.pm10;
        doc["co2"] = f.co2; doc["voc"] = f.voc; doc["nox"] = f.nox;
        doc["temp"] = f.temp; doc["rh"] = f.rh;
        // v4 — 0xFFFF is the sensor's "unknown"; omit rather than send a fake number
        auto pcOut = [&](const char* k, uint16_t v) { if (v != 0xFFFF) doc[k] = v / 10.0f; };
        pcOut("pc05", f.pc05); pcOut("pc1", f.pc1); pcOut("pc25", f.pc25);
        pcOut("pc4", f.pc4);   pcOut("pc10", f.pc10);
        if (f.voc_raw != 0xFFFF) doc["voc_raw"] = f.voc_raw;
        if (f.nox_raw != 0xFFFF) doc["nox_raw"] = f.nox_raw;
    } else if (f.status[GRP_SEN66] == FS_INVALID) {
        for (const char* k : {"pm1","pm25","pm4","pm10","co2","voc","nox","temp","rh",
                              "pc05","pc1","pc25","pc4","pc10","voc_raw","nox_raw"}) jsonNull(doc, k);
    }
    if (emitSlow(f.status[GRP_BH1750]))          doc["lux"] = f.lux;
    else if (f.status[GRP_BH1750] == FS_INVALID) jsonNull(doc, "lux");
    if (emitSlow(f.status[GRP_BME])) {
        doc["pressure_hpa"] = f.pressure; doc["bme_temp"] = f.bme_temp; doc["bme_rh"] = f.bme_rh;
    } else if (f.status[GRP_BME] == FS_INVALID) {
        for (const char* k : {"pressure_hpa","bme_temp","bme_rh"}) jsonNull(doc, k);
    }
    if (f.has_adxl) {
        doc["rumble"]      = f.rumble_rms;
        doc["rumble_peak"] = f.rumble_peak;
        doc["accel_mag"]   = f.accel_mag;
        // v2 — vibration / infrasound
        doc["ppv_mm_s"]    = roundf(f.ppv_m_s * 10000.0f) / 10.0f;  // m/s → mm/s, 1 dp
        doc["accel_dom_hz"] = f.accel_dom_hz;
        // 1/3-octave bands: 4, 8, 16, 31.5, 63, 125 Hz in dBm/s²
        static const char* BAND_KEYS[REC_ACCEL_BANDS] =
            {"vib_4hz","vib_8hz","vib_16hz","vib_31hz","vib_63hz","vib_125hz"};
        for (int b = 0; b < REC_ACCEL_BANDS; ++b) doc[BAND_KEYS[b]] = f.accel_band_db[b];
    } else if (f.status[GRP_ADXL] == FS_INVALID) {
        for (const char* k : {"rumble","rumble_peak","accel_mag","ppv_mm_s","accel_dom_hz",
                              "vib_4hz","vib_8hz","vib_16hz","vib_31hz","vib_63hz","vib_125hz"})
            jsonNull(doc, k);
    }
    if (emitSlow(f.status[GRP_CO])) {
        doc["co_mv"] = f.co_mv; doc["co_rs"] = gasRsLocal(f.co_mv, GAS_CO_RL_OHMS);
    } else if (f.status[GRP_CO] == FS_INVALID) {
        for (const char* k : {"co_mv","co_rs"}) jsonNull(doc, k);
    }
    if (emitSlow(f.status[GRP_HCHO])) {
        doc["hcho_mv"] = f.hcho_mv; doc["hcho_rs"] = gasRsLocal(f.hcho_mv, GAS_HCHO_RL_OHMS);
    } else if (f.status[GRP_HCHO] == FS_INVALID) {
        for (const char* k : {"hcho_mv","hcho_rs"}) jsonNull(doc, k);
    }
    if (emitSlow(f.status[GRP_SOIL])) {
        float pct = 100.0f * (float)(SOIL_DRY_MV - (int)f.soil_mv) / (float)(SOIL_DRY_MV - SOIL_WET_MV);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        doc["soil_mv"] = f.soil_mv; doc["soil_pct"] = pct;
    } else if (f.status[GRP_SOIL] == FS_INVALID) {
        for (const char* k : {"soil_mv","soil_pct"}) jsonNull(doc, k);
    }
    if (emitSlow(f.status[GRP_BATTERY])) {
        doc["bat_raw_mv"] = f.bat_raw_mv; doc["bat_cal"] = f.bat_cal;
        if (f.bat_cal) {
            float v = f.bat_raw_mv / 1000.0f * BAT_DIVIDER_FACTOR;
            float pct = (v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            doc["bat_v"] = v; doc["bat_pct"] = pct;
        }
    } else if (f.status[GRP_BATTERY] == FS_INVALID) {
        for (const char* k : {"bat_raw_mv","bat_v","bat_pct"}) jsonNull(doc, k);
    }
    if (f.has_mic) {
        doc["noise_dba"]  = f.noise_dba;      // LAeq over capture window, dB(A)
        doc["noise_spl"]  = f.noise_spl;
        doc["noise_dbfs"] = f.noise_dbfs;
        doc["noise_clip"] = f.noise_clip;
        // v2 — Dutch/EU aircraft noise metrics
        doc["lamax"]      = f.noise_lamax;    // peak A-weighted in ~1.3 s window, dB(A)
        doc["lceq"]       = f.noise_lceq;     // C-weighted equivalent, dB(C)
        doc["lc_minus_la"] = roundf((f.noise_lceq - f.noise_dba) * 10.0f) / 10.0f;
        JsonArray bands = doc["noise_bands"].to<JsonArray>();
        for (int b = 0; b < REC_NBANDS; ++b) bands.add(f.bands[b]);
    }
}
