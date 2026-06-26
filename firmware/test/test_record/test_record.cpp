#include <unity.h>
#include <math.h>
#include "record.h"
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
    f.has_bh1750 = true; f.lux = 333;
    f.has_bme = true; f.pressure = 1013.2f; f.bme_temp = 21.5f; f.bme_rh = 49.0f;
    f.has_adxl = true; f.rumble_rms = 0.123f; f.rumble_peak = 0.987f; f.accel_mag = 9.81f;
    f.has_co = true; f.co_mv = 410;
    f.has_hcho = true; f.hcho_mv = 222;
    f.has_soil = true; f.soil_mv = 1800;
    f.has_battery = true; f.bat_raw_mv = 2750;
    f.has_mic = true; f.noise_dba = 41.2f; f.noise_spl = 55.5f; f.noise_dbfs = -38.4f;
    for (int b = 0; b < REC_NBANDS; ++b) f.bands[b] = 20.0f + b;
    return f;
}

void test_size_is_71() { TEST_ASSERT_EQUAL_UINT(71, sizeof(Record)); }

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
}

void test_absent_sensors_clear_present_bits() {
    RecordFields in;      // all has_* default false
    in.seq = 1; in.ts = 1719400000;
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_SEN66);
    TEST_ASSERT_EQUAL_UINT16(0, r.flags & PRESENT_MIC);
}

void test_quantization_clamps_high_values() {
    RecordFields in; in.has_sen66 = true; in.pm25 = 1e9f;   // absurd
    Record r = record_pack(in);
    TEST_ASSERT_EQUAL_UINT16(65535, r.pm25);                // clamped, no wrap
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
    RUN_TEST(test_size_is_71);
    RUN_TEST(test_roundtrip_preserves_within_quantization);
    RUN_TEST(test_absent_sensors_clear_present_bits);
    RUN_TEST(test_quantization_clamps_high_values);
    RUN_TEST(test_to_json_has_expected_keys_and_derived);
    RUN_TEST(test_to_json_omits_absent_sensor_keys);
    return UNITY_END();
}
