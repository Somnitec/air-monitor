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
static uint8_t qU8(float v) {
    float s = roundf(v);
    if (s > 255.0f) return 255;
    if (s < 0.0f)   return 0;
    return (uint8_t)s;
}
static void setBit(uint16_t& flags, uint16_t bit, bool on) { if (on) flags |= bit; }

Record record_pack(const RecordFields& f) {
    Record r;
    memset(&r, 0, sizeof(r));
    r.seq = f.seq; r.ts = f.ts; r.up_ms = f.up_ms; r.boot = f.boot;

    uint16_t flags = 0;
    setBit(flags, FLAG_TS_OK,      f.ts_ok);
    setBit(flags, FLAG_BAT_CAL,    f.bat_cal);
    setBit(flags, FLAG_NOISE_CLIP, f.noise_clip);
    setBit(flags, PRESENT_SEN66,   f.has_sen66);
    setBit(flags, PRESENT_BH1750,  f.has_bh1750);
    setBit(flags, PRESENT_BME,     f.has_bme);
    setBit(flags, PRESENT_ADXL,    f.has_adxl);
    setBit(flags, PRESENT_CO,      f.has_co);
    setBit(flags, PRESENT_HCHO,    f.has_hcho);
    setBit(flags, PRESENT_SOIL,    f.has_soil);
    setBit(flags, PRESENT_BATTERY, f.has_battery);
    setBit(flags, PRESENT_MIC,     f.has_mic);
    r.flags = flags;

    if (f.has_sen66) {
        r.pm1 = qU16(f.pm1, 10); r.pm25 = qU16(f.pm25, 10);
        r.pm4 = qU16(f.pm4, 10); r.pm10 = qU16(f.pm10, 10);
        r.co2 = f.co2; r.voc = qU16(f.voc, 1); r.nox = qU16(f.nox, 1);
        r.temp = qI16(f.temp, 100); r.rh = qU16(f.rh, 100);
    }
    if (f.has_bh1750) r.lux = qU16(f.lux, 1);
    if (f.has_bme) {
        r.pressure = qU16(f.pressure, 10);
        r.bme_temp = qI16(f.bme_temp, 100); r.bme_rh = qU16(f.bme_rh, 100);
    }
    if (f.has_adxl) {
        r.rumble_rms = qU16(f.rumble_rms, 1000);
        r.rumble_peak = qU16(f.rumble_peak, 1000);
        r.accel_mag = qU16(f.accel_mag, 100);
    }
    if (f.has_co)      r.co_mv = f.co_mv;
    if (f.has_hcho)    r.hcho_mv = f.hcho_mv;
    if (f.has_soil)    r.soil_mv = f.soil_mv;
    if (f.has_battery) r.bat_raw_mv = f.bat_raw_mv;
    if (f.has_mic) {
        r.noise_dba = qI16(f.noise_dba, 10);
        r.noise_spl = qI16(f.noise_spl, 10);
        r.noise_dbfs = qI16(f.noise_dbfs, 10);
        for (int b = 0; b < REC_NBANDS; ++b) r.bands[b] = qU8(f.bands[b]);
    }
    return r;
}

void record_unpack(const Record& r, RecordFields& f) {
    f.seq = r.seq; f.ts = r.ts; f.up_ms = r.up_ms; f.boot = r.boot;
    f.ts_ok      = r.flags & FLAG_TS_OK;
    f.bat_cal    = r.flags & FLAG_BAT_CAL;
    f.noise_clip = r.flags & FLAG_NOISE_CLIP;

    f.has_sen66 = r.flags & PRESENT_SEN66;
    if (f.has_sen66) {
        f.pm1 = r.pm1/10.0f; f.pm25 = r.pm25/10.0f; f.pm4 = r.pm4/10.0f; f.pm10 = r.pm10/10.0f;
        f.co2 = r.co2; f.voc = r.voc; f.nox = r.nox;
        f.temp = r.temp/100.0f; f.rh = r.rh/100.0f;
    }
    f.has_bh1750 = r.flags & PRESENT_BH1750; if (f.has_bh1750) f.lux = r.lux;
    f.has_bme = r.flags & PRESENT_BME;
    if (f.has_bme) { f.pressure = r.pressure/10.0f; f.bme_temp = r.bme_temp/100.0f; f.bme_rh = r.bme_rh/100.0f; }
    f.has_adxl = r.flags & PRESENT_ADXL;
    if (f.has_adxl) { f.rumble_rms = r.rumble_rms/1000.0f; f.rumble_peak = r.rumble_peak/1000.0f; f.accel_mag = r.accel_mag/100.0f; }
    f.has_co = r.flags & PRESENT_CO;       if (f.has_co)      f.co_mv = r.co_mv;
    f.has_hcho = r.flags & PRESENT_HCHO;   if (f.has_hcho)    f.hcho_mv = r.hcho_mv;
    f.has_soil = r.flags & PRESENT_SOIL;   if (f.has_soil)    f.soil_mv = r.soil_mv;
    f.has_battery = r.flags & PRESENT_BATTERY; if (f.has_battery) f.bat_raw_mv = r.bat_raw_mv;
    f.has_mic = r.flags & PRESENT_MIC;
    if (f.has_mic) {
        f.noise_dba = r.noise_dba/10.0f; f.noise_spl = r.noise_spl/10.0f; f.noise_dbfs = r.noise_dbfs/10.0f;
        for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = r.bands[b];
    }
}

// MEMS MOS resistance from the divider — mirrors firmware.cpp gasRs().
static float gasRsLocal(float vout_mv, float rl_ohms) {
    if (vout_mv < 1.0f) return INFINITY;
    return rl_ohms * (GAS_VCC_MV - vout_mv) / vout_mv;
}

void record_to_json(const Record& r, JsonDocument& doc) {
    RecordFields f; record_unpack(r, f);

    doc["ts"]    = f.ts;
    doc["ts_ok"] = f.ts_ok;
    doc["dev"]   = DEVICE_ID;
    doc["up_ms"] = f.up_ms;
    doc["boot"]  = f.boot;

    if (f.has_sen66) {
        doc["pm1"] = f.pm1; doc["pm25"] = f.pm25; doc["pm4"] = f.pm4; doc["pm10"] = f.pm10;
        doc["co2"] = f.co2; doc["voc"] = f.voc; doc["nox"] = f.nox;
        doc["temp"] = f.temp; doc["rh"] = f.rh;
    }
    if (f.has_bh1750) doc["lux"] = f.lux;
    if (f.has_bme) {
        doc["pressure_hpa"] = f.pressure; doc["bme_temp"] = f.bme_temp; doc["bme_rh"] = f.bme_rh;
    }
    if (f.has_adxl) {
        doc["rumble"] = f.rumble_rms; doc["rumble_peak"] = f.rumble_peak; doc["accel_mag"] = f.accel_mag;
    }
    if (f.has_co) {
        doc["co_mv"] = f.co_mv; doc["co_rs"] = gasRsLocal(f.co_mv, GAS_CO_RL_OHMS);
    }
    if (f.has_hcho) {
        doc["hcho_mv"] = f.hcho_mv; doc["hcho_rs"] = gasRsLocal(f.hcho_mv, GAS_HCHO_RL_OHMS);
    }
    if (f.has_soil) {
        float pct = 100.0f * (float)(SOIL_DRY_MV - (int)f.soil_mv) / (float)(SOIL_DRY_MV - SOIL_WET_MV);
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        doc["soil_mv"] = f.soil_mv; doc["soil_pct"] = pct;
    }
    if (f.has_battery) {
        doc["bat_raw_mv"] = f.bat_raw_mv; doc["bat_cal"] = f.bat_cal;
        if (f.bat_cal) {
            float v = f.bat_raw_mv / 1000.0f * BAT_DIVIDER_FACTOR;
            float pct = (v - BAT_EMPTY_V) / (BAT_FULL_V - BAT_EMPTY_V) * 100.0f;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
            doc["bat_v"] = v; doc["bat_pct"] = pct;
        }
    }
    if (f.has_mic) {
        doc["noise_dba"] = f.noise_dba; doc["noise_spl"] = f.noise_spl;
        doc["noise_dbfs"] = f.noise_dbfs; doc["noise_clip"] = f.noise_clip;
        JsonArray bands = doc["noise_bands"].to<JsonArray>();
        for (int b = 0; b < REC_NBANDS; ++b) bands.add(f.bands[b]);
    }
}
