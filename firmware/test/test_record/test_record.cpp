#include <unity.h>
#include <math.h>
#include <string>
#include "record.h"   // pulls in ArduinoJson.h
#include "config.h"   // DEVICE_ID, GAS_CO_RL_OHMS, GAS_VCC_MV for the to_json checks

void setUp() {}
void tearDown() {}

static RecordFields sample() {
    RecordFields f;
    f.seq = 42; f.ts = 1719400000; f.up_ms = 123456; f.boot = 0xBEEF;
    f.ts_ok = true; f.bat_cal = true; f.noise_clip = false;
    f.has_sen66 = true;
    f.pm1 = 1.2f; f.pm25 = 3.4f; f.pm4 = 5.6f; f.pm10 = 7.8f;
    f.co2 = 812; f.voc = 101; f.nox = 3; f.temp = 21.37f; f.rh = 48.5f;
    // v4: sensor-encoded (x10 / raw ticks)
    f.pc05 = 1234; f.pc1 = 1300; f.pc25 = 1350; f.pc4 = 1360; f.pc10 = 1365;
    f.voc_raw = 28901; f.nox_raw = 17444;
    f.has_bh1750 = true; f.lux = 333;
    f.has_bme = true; f.pressure = 1013.2f; f.bme_temp = 21.5f; f.bme_rh = 49.0f;
    f.has_adxl = true;
    f.rumble_rms = 0.123f; f.rumble_peak = 0.987f; f.accel_mag = 9.81f;
    // v2 accel
    f.ppv_m_s     = 0.0012f;    // 0.0012 m/s = 1.2 mm/s
    f.accel_dom_hz = 16;
    for (int b = 0; b < REC_ACCEL_BANDS; ++b) f.accel_band_db[b] = -60.0f + b * 5;
    f.has_co = true; f.co_mv = 410;
    f.has_hcho = true; f.hcho_mv = 222;
    f.has_soil = true; f.soil_mv = 1800;
    f.has_battery = true; f.bat_raw_mv = 2750;
    f.has_mic = true; f.noise_dba = 41.2f; f.noise_spl = 55.5f; f.noise_dbfs = -38.4f;
    // v2 mic
    f.noise_lamax = 46.8f;
    f.noise_lceq  = 48.3f;
    for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = 20.0f + b;
    // v3: every group present & freshly read in this fixture.
    for (int g = 0; g < REC_NGROUPS; ++g) f.status[g] = FS_OK;
    return f;
}

void test_size_is_102() { TEST_ASSERT_EQUAL_UINT(102, sizeof(Record)); }

void test_roundtrip_preserves_within_quantization() {
    RecordFields in = sample();
    Record r = record_pack(in);
    RecordFields out;
    record_unpack(r, out);

    TEST_ASSERT_EQUAL_UINT32(in.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT32(in.ts, out.ts);
    TEST_ASSERT_EQUAL_UINT16(in.boot, out.boot);
    TEST_ASSERT_TRUE(out.ts_ok);
    TEST_ASSERT_TRUE(out.bat_cal);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.pm25, out.pm25);   // x10 scale -> 0.1 resolution
    TEST_ASSERT_EQUAL_UINT16(in.co2, out.co2);
    TEST_ASSERT_FLOAT_WITHIN(0.005f, in.temp, out.temp);  // x100 -> 0.01 resolution
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.pressure, out.pressure);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, in.rumble_rms, out.rumble_rms);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.noise_dbfs, out.noise_dbfs);
    TEST_ASSERT_EQUAL_UINT16(in.bat_raw_mv, out.bat_raw_mv);
    TEST_ASSERT_FLOAT_WITHIN(0.6f, in.bands[5], out.bands[5]);  // u8 dB rounding
    // v4: sensor-encoded values pass through exactly
    TEST_ASSERT_EQUAL_UINT16(in.pc05, out.pc05);
    TEST_ASSERT_EQUAL_UINT16(in.pc10, out.pc10);
    TEST_ASSERT_EQUAL_UINT16(in.voc_raw, out.voc_raw);
    TEST_ASSERT_EQUAL_UINT16(in.nox_raw, out.nox_raw);
}

// ---- v4: SEN66 number concentrations + raw ticks ----

void test_v4_json_emits_scaled_pc_and_raw_ticks() {
    RecordFields in = sample();
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.4f, doc["pc05"].as<float>());  // 1234 x10 → 123.4 #/cm³
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 136.5f, doc["pc10"].as<float>());
    TEST_ASSERT_EQUAL_UINT(28901, doc["voc_raw"].as<unsigned>());      // ticks, unscaled
    TEST_ASSERT_EQUAL_UINT(17444, doc["nox_raw"].as<unsigned>());
}

void test_v4_unknown_sentinel_omits_keys() {
    RecordFields in = sample();
    in.pc05 = in.pc1 = in.pc25 = in.pc4 = in.pc10 = 0xFFFF;   // sensor "unknown"
    in.voc_raw = 0xFFFF;
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    TEST_ASSERT_FALSE(doc["pc05"].is<float>());
    TEST_ASSERT_FALSE(doc["voc_raw"].is<unsigned>());
    TEST_ASSERT_TRUE(doc["nox_raw"].is<unsigned>());          // only the unknown ones drop
    TEST_ASSERT_TRUE(doc["pm25"].is<float>());                // rest of SEN66 unaffected
}

void test_v4_invalid_group_nulls_pc_keys() {
    RecordFields in = sample();
    in.status[GRP_SEN66] = FS_INVALID;
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    char buf[2048]; serializeJson(doc, buf, sizeof(buf));
    std::string s(buf);
    TEST_ASSERT_TRUE(s.find("\"pc05\":null") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("\"voc_raw\":null") != std::string::npos);
}

void test_absent_sensors_clear_present_bits() {
    RecordFields in;      // all has_* default false
    in.seq = 1; in.ts = 1719400000;
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_SEN66);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_MIC);
}

void test_quantization_clamps_high_values() {
    RecordFields in; in.status[GRP_SEN66] = FS_OK; in.pm25 = 1e9f;   // absurd
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(65535, r.pm25);                // clamped, no wrap
}

// ---- v3: per-group tri-state status (unchanged / invalid / absent) ----

void test_status_roundtrip() {
    RecordFields in = sample();
    in.status[GRP_SEN66]   = FS_OK;
    in.status[GRP_BME]     = FS_UNCHANGED;   // value carried, present bit still set
    in.status[GRP_BH1750]  = FS_INVALID;     // failed read
    in.status[GRP_SOIL]    = FS_ABSENT;      // not installed
    Record r = record_pack(in);
    RecordFields out; record_unpack(r, out);
    TEST_ASSERT_EQUAL_UINT8(FS_OK,        out.status[GRP_SEN66]);
    TEST_ASSERT_EQUAL_UINT8(FS_UNCHANGED, out.status[GRP_BME]);
    TEST_ASSERT_EQUAL_UINT8(FS_INVALID,   out.status[GRP_BH1750]);
    TEST_ASSERT_EQUAL_UINT8(FS_ABSENT,    out.status[GRP_SOIL]);
    // present bit reflects "installed" (everything except the absent one)
    TEST_ASSERT_TRUE(r.flags & PRESENT_BME);
    TEST_ASSERT_FALSE(r.flags & PRESENT_SOIL);
    // FS_UNCHANGED still carries its value through the binary record
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.pressure, out.pressure);
}

// Serialize to a string so we can distinguish "key omitted" from "key: null".
static std::string toJsonStr(const RecordFields& in) {
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    char buf[2048]; serializeJson(doc, buf, sizeof(buf));
    return std::string(buf);
}

void test_to_json_unchanged_omits_key() {
    RecordFields in = sample();
    in.status[GRP_SEN66] = FS_UNCHANGED;        // carried forward -> omit on the wire
    std::string s = toJsonStr(in);
    TEST_ASSERT_TRUE(s.find("\"pm25\"") == std::string::npos);   // key absent
}

void test_to_json_invalid_emits_null() {
    RecordFields in = sample();
    in.status[GRP_SEN66] = FS_INVALID;          // real gap -> explicit null
    std::string s = toJsonStr(in);
    TEST_ASSERT_TRUE(s.find("\"pm25\":null") != std::string::npos);
}

void test_to_json_ok_emits_value() {
    RecordFields in = sample();
    in.status[GRP_SEN66] = FS_OK;
    std::string s = toJsonStr(in);
    TEST_ASSERT_TRUE(s.find("\"pm25\":null") == std::string::npos);
    TEST_ASSERT_TRUE(s.find("\"pm25\"") != std::string::npos);   // present with a value
}

void test_v2_mic_roundtrip() {
    RecordFields in = sample();
    Record r = record_pack(in);
    RecordFields out; record_unpack(r, out);

    // LAmax: int16 × 10 → 0.1 dB resolution
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.noise_lamax, out.noise_lamax);
    // LCeq: same
    TEST_ASSERT_FLOAT_WITHIN(0.05f, in.noise_lceq, out.noise_lceq);
}

void test_v2_accel_roundtrip() {
    RecordFields in = sample();
    Record r = record_pack(in);
    RecordFields out; record_unpack(r, out);

    // PPV: stored as 0.1 mm/s (ppv_mm10 = ppv_m_s × 10000)
    // in.ppv_m_s=0.0012 → 12 units → 0.0012 m/s out (exact)
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, in.ppv_m_s, out.ppv_m_s);

    // dom_freq stored as uint8
    TEST_ASSERT_EQUAL_UINT8(in.accel_dom_hz, out.accel_dom_hz);

    // Band levels: stored as int8 (1 dB resolution) → within 0.5 dB
    for (int b = 0; b < REC_ACCEL_BANDS; ++b)
        TEST_ASSERT_FLOAT_WITHIN(0.5f, in.accel_band_db[b], out.accel_band_db[b]);
}

#include <ArduinoJson.h>

void test_to_json_has_expected_keys_and_derived() {
    RecordFields in = sample();
    Record r = record_pack(in);
    JsonDocument doc;
    record_to_json(r, doc);

    TEST_ASSERT_TRUE(doc["ts"].is<uint32_t>());
    TEST_ASSERT_EQUAL_STRING(DEVICE_ID, doc["dev"]);
    TEST_ASSERT_TRUE(doc["co_rs"].is<float>());          // derived from co_mv
    TEST_ASSERT_TRUE(doc["soil_pct"].is<float>());       // derived from soil_mv
    TEST_ASSERT_TRUE(doc["bat_v"].is<float>());          // derived (bat_cal=true)
    TEST_ASSERT_TRUE(doc["noise_bands"].is<JsonArray>());
    TEST_ASSERT_EQUAL_UINT(REC_NBANDS, doc["noise_bands"].as<JsonArray>().size());

    // co_rs must equal RL*(Vcc-v)/v with v=410mV, RL=GAS_CO_RL_OHMS
    float expect = GAS_CO_RL_OHMS * (GAS_VCC_MV - 410.0f) / 410.0f;
    TEST_ASSERT_FLOAT_WITHIN(1.0f, expect, doc["co_rs"].as<float>());

    // v2 keys must be present
    TEST_ASSERT_TRUE(doc["lamax"].is<float>());
    TEST_ASSERT_TRUE(doc["lceq"].is<float>());
    TEST_ASSERT_TRUE(doc["lc_minus_la"].is<float>());
    TEST_ASSERT_TRUE(doc["ppv_mm_s"].is<float>());
    TEST_ASSERT_TRUE(doc["vib_4hz"].is<float>());
    TEST_ASSERT_TRUE(doc["vib_125hz"].is<float>());
}

void test_to_json_lc_minus_la_value() {
    RecordFields in = sample();  // lamax=46.8, lceq=48.3, laeq=41.2
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    // lc_minus_la = lceq - laeq = 48.3 - 41.2 = 7.1 dB (±quantization error)
    TEST_ASSERT_FLOAT_WITHIN(0.2f, 7.1f, doc["lc_minus_la"].as<float>());
}

void test_to_json_omits_absent_sensor_keys() {
    RecordFields in; in.seq = 1; in.ts = 1719400000;   // no sensors present
    Record r = record_pack(in);
    JsonDocument doc; record_to_json(r, doc);
    TEST_ASSERT_FALSE(doc["pm25"].is<float>());
    TEST_ASSERT_FALSE(doc["noise_bands"].is<JsonArray>());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_size_is_102);
    RUN_TEST(test_roundtrip_preserves_within_quantization);
    RUN_TEST(test_v4_json_emits_scaled_pc_and_raw_ticks);
    RUN_TEST(test_v4_unknown_sentinel_omits_keys);
    RUN_TEST(test_v4_invalid_group_nulls_pc_keys);
    RUN_TEST(test_absent_sensors_clear_present_bits);
    RUN_TEST(test_quantization_clamps_high_values);
    RUN_TEST(test_status_roundtrip);
    RUN_TEST(test_to_json_unchanged_omits_key);
    RUN_TEST(test_to_json_invalid_emits_null);
    RUN_TEST(test_to_json_ok_emits_value);
    RUN_TEST(test_v2_mic_roundtrip);
    RUN_TEST(test_v2_accel_roundtrip);
    RUN_TEST(test_to_json_has_expected_keys_and_derived);
    RUN_TEST(test_to_json_lc_minus_la_value);
    RUN_TEST(test_to_json_omits_absent_sensor_keys);
    return UNITY_END();
}
