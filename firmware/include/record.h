#pragma once
// Dense binary record for the on-device FIFO ring. 84 bytes vs ~400 bytes of
// JSON. Stored verbatim in the ring; expanded back to the existing JSON shape
// only at sync time (record_to_json), so the PC contract is unchanged.
//
// Schema v2 adds Dutch/EU aircraft noise metrics (LAmax, LCeq) and infrasound
// vibration metrics (PPV, 1/3-oct accel bands, dominant frequency).
//
// Schema v4 adds the SEN66 outputs we weren't draining: particle number
// concentrations (PC0.5..PC10, #/cm³ — closest this hardware gets to aircraft
// ultrafines) and raw SGP41 VOC/NOx ticks (the index self-normalizes and erases
// long-term trends; raw ticks keep them). All seven use the sensor's own 0xFFFF
// "unknown" sentinel verbatim instead of a status bit.
//
// Quantization scales are chosen so resolution far exceeds each sensor's real
// accuracy. Absent sensors store 0 and clear their present bit.

#include <stdint.h>
#include <ArduinoJson.h>   // header-only; record_to_json takes a real JsonDocument

// v4: SEN66 number conc. + raw gas ticks. v5: layout identical to v4 — bumped only to
// force a queue wipe on units whose v4 flash resumed into mixed-stride segments (the
// wipeQueue basename bug had re-stamped their fmt file as v4 without deleting anything).
static constexpr uint8_t  RECORD_SCHEMA_VERSION = 5;
static constexpr int      REC_NBANDS      = 9;   // must match MIC_NBANDS
static constexpr int      REC_ACCEL_BANDS = 6;   // must match ACCEL_NBANDS: 4,8,16,31.5,63,125 Hz

// ---- per-sensor-group status (v3) ----
// Each group records one of four states so the server can tell "no new value"
// (carry the last one forward) from "sensor returned bad data" (a real gap, NULL)
// from "sensor not installed" (omit). Packed 2 bits per group into Record.status2.
enum FieldStatus : uint8_t {
    FS_ABSENT    = 0,   // sensor not present on this unit
    FS_OK        = 1,   // freshly read this record, value valid
    FS_UNCHANGED = 2,   // not re-read this cycle — value carried forward (omit on wire)
    FS_INVALID   = 3,   // present but read failed / out of range — a real gap (null on wire)
};
enum RecGroup : uint8_t {
    GRP_SEN66 = 0, GRP_BH1750, GRP_BME, GRP_ADXL,
    GRP_CO, GRP_HCHO, GRP_SOIL, GRP_BATTERY, GRP_MIC,
    REC_NGROUPS                                   // = 9; uses 18 bits of status2
};

// ---- flag bits ----
enum : uint16_t {
    FLAG_TS_OK       = 1u << 0,
    FLAG_BAT_CAL     = 1u << 1,
    FLAG_NOISE_CLIP  = 1u << 2,
    PRESENT_SEN66    = 1u << 3,
    PRESENT_BH1750   = 1u << 4,
    PRESENT_BME      = 1u << 5,
    PRESENT_ADXL     = 1u << 6,
    PRESENT_CO       = 1u << 7,
    PRESENT_HCHO     = 1u << 8,
    PRESENT_SOIL     = 1u << 9,
    PRESENT_BATTERY  = 1u << 10,
    PRESENT_MIC      = 1u << 11,
    // bits 12-15 reserved
};

#pragma pack(push, 1)
struct Record {
    uint32_t seq;          // monotonic record number
    uint32_t ts;           // unix epoch (UTC); < EPOCH_VALID_AFTER => clock unsynced
    uint32_t up_ms;        // millis() at capture
    uint16_t flags;        // FLAG_* | PRESENT_* (present bit = status != FS_ABSENT)
    uint16_t boot;         // low 16 bits of per-boot id
    uint32_t status2;      // v3: FieldStatus per RecGroup, 2 bits each (bits 2*g, 2*g+1)

    uint16_t pm1, pm25, pm4, pm10;  // ug/m3 x10
    uint16_t co2;                   // ppm
    uint16_t voc, nox;              // index x1
    int16_t  temp;                  // C x100
    uint16_t rh;                    // %RH x100

    uint16_t lux;                   // lux x1
    uint16_t pressure;              // hPa x10
    int16_t  bme_temp;              // C x100
    uint16_t bme_rh;                // %RH x100

    uint16_t rumble_rms;            // m/s^2 x1000
    uint16_t rumble_peak;           // m/s^2 x1000
    uint16_t accel_mag;             // m/s^2 x100

    uint16_t co_mv;                 // mV
    uint16_t hcho_mv;               // mV
    uint16_t soil_mv;               // mV
    uint16_t bat_raw_mv;            // mV

    int16_t  noise_dba;             // dB(A) x10  — LAeq over capture window
    int16_t  noise_spl;             // dB x10
    int16_t  noise_dbfs;            // dBFS x10
    uint8_t  bands[REC_NBANDS];     // dB(A) rounded, clamped 0..255

    // ---- v2: aviation / infrasound metrics ----
    int16_t  noise_lamax;           // dB(A) x10  — peak A-weighted level in ~1.3 s capture
    int16_t  noise_lceq;            // dB(C) x10  — C-weighted LAeq (NL: C−A > 6 dB flags LF noise)
    uint16_t ppv_mm10;              // Peak Particle Velocity, 0.1 mm/s units (max 6553 mm/s)
    uint8_t  accel_dom_hz;          // dominant vibration frequency 1–255 Hz (0 = no signal)
    int8_t   accel_bands[REC_ACCEL_BANDS]; // 1/3-oct accel levels, dB re 1 m/s² (dBm/s²)

    // ---- v4: SEN66 number concentrations + raw gas ticks ----
    uint16_t pc05, pc1, pc25, pc4, pc10;  // particles/cm³ x10; 0xFFFF = unknown
    uint16_t voc_raw, nox_raw;            // SGP41 raw ticks; 0xFFFF = unknown
};
#pragma pack(pop)

static constexpr unsigned RECORD_SIZE = 102;
static_assert(sizeof(Record) == RECORD_SIZE, "Record must be tightly packed to 102 bytes");

// Plain (Arduino-free) inputs to pack. firmware.cpp fills this from live sensors.
struct RecordFields {
    uint32_t seq = 0, ts = 0, up_ms = 0;
    uint16_t boot = 0;
    bool ts_ok = false, bat_cal = false, noise_clip = false;

    // Per-group state (v3). Authoritative source for pack/unpack/json; the has_*
    // bools below are kept as a convenience mirror (true when a value is available,
    // i.e. status is FS_OK or FS_UNCHANGED) for the serial snapshot printers.
    uint8_t status[REC_NGROUPS] = { FS_ABSENT, FS_ABSENT, FS_ABSENT, FS_ABSENT,
                                    FS_ABSENT, FS_ABSENT, FS_ABSENT, FS_ABSENT, FS_ABSENT };

    bool  has_sen66 = false;
    float pm1 = 0, pm25 = 0, pm4 = 0, pm10 = 0, voc = 0, nox = 0, temp = 0, rh = 0;
    uint16_t co2 = 0;
    // v4 — kept in the sensor's own encoding (x10 / raw ticks, 0xFFFF = unknown)
    uint16_t pc05 = 0xFFFF, pc1 = 0xFFFF, pc25 = 0xFFFF, pc4 = 0xFFFF, pc10 = 0xFFFF;
    uint16_t voc_raw = 0xFFFF, nox_raw = 0xFFFF;

    bool  has_bh1750 = false; float lux = 0;
    bool  has_bme = false;    float pressure = 0, bme_temp = 0, bme_rh = 0;
    bool  has_adxl = false;   float rumble_rms = 0, rumble_peak = 0, accel_mag = 0;
    bool  has_co = false;     uint16_t co_mv = 0;
    bool  has_hcho = false;   uint16_t hcho_mv = 0;
    bool  has_soil = false;   uint16_t soil_mv = 0;
    bool  has_battery = false; uint16_t bat_raw_mv = 0;
    bool  has_mic = false;    float noise_dba = 0, noise_spl = 0, noise_dbfs = 0;
    float bands[REC_NBANDS] = {0};

    // v2 — added alongside v1 fields; zero if sensor absent
    float    noise_lamax = 0;                    // dB(A) — max in capture window
    float    noise_lceq  = 0;                    // dB(C) — C-weighted equivalent
    float    ppv_m_s     = 0;                    // m/s   — peak particle velocity
    uint8_t  accel_dom_hz = 0;                   // Hz    — dominant vibration frequency
    float    accel_band_db[REC_ACCEL_BANDS] = {0}; // dBm/s² per 1/3-oct band
};

Record record_pack(const RecordFields& f);
void    record_unpack(const Record& r, RecordFields& out);   // inverse (lossy by quantization)

// Expand a record into the JSON field set, re-deriving co_rs/hcho_rs,
// soil_pct, bat_v/bat_pct, ppv_mm_s from config constants.
//
// full_slow: normally the slow channel is delta-encoded — FS_UNCHANGED groups omit
// their keys and the server carries the last value forward. Set full_slow=true to
// instead emit the carried-forward values in full (treat FS_UNCHANGED like FS_OK).
// Used for the first record of each sync batch so a reconnect backlog reconstructs
// correctly even when the server's forward-fill cache is cold (restart / fresh DB).
void    record_to_json(const Record& r, JsonDocument& doc, bool full_slow = false);
