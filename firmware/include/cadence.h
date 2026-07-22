#pragma once
// Adaptive storage cadence (pure logic — Arduino-free, host-tested).
//
// The mic is the primary instrument and only "listens" ~1.3 s per capture, so the
// device captures noise as fast as it can; this module decides which captures are
// worth *storing*. When the soundscape is changing fast (an aircraft approaching),
// it densifies — store every capture; when quiet and steady it stores only at a
// baseline interval to keep the row count (and DB) small. See DESIGN.md / the
// cadence-redesign note for why this matters to the noise study.

#include <stdint.h>

struct CadenceParams {
    float    densify_delta_dba;  // |Δ LAeq| between captures that arms densification
    uint32_t densify_hold_ms;    // densified window persists this long after each trigger
    uint32_t quiet_store_ms;     // when not densified, store at most this often
    float    densify_abs_dba;    // LAeq at/above this arms densification outright.
                                 // The delta test alone misses aircraft: captures are
                                 // ~1.3 s apart and a flyover ramps ~0.5 dB/capture, so
                                 // only impulsive noises ever jumped 6 dB (measured
                                 // 2026-07-14: median flyover stored at pure baseline
                                 // cadence, 9 records in 90 s).
};

struct CadenceState {
    float    last_noise   = 0.0f;  // previous capture's LAeq (for the delta test)
    bool     have_last    = false; // false until the first noise capture is seen
    uint32_t last_store_ms = 0;    // millis() of the last stored record
    uint32_t densify_until_ms = 0; // densified while now < this
    bool     primed       = false; // false until the very first decision (forces a store)
};

struct CadenceDecision {
    bool store;       // push this capture to the ring?
    bool densified;   // currently inside a densified (high-rate) window?
};

// Decide whether to store the current capture. `has_noise` is false when the mic
// produced no value this cycle (then the delta test is skipped). Updates `st`.
CadenceDecision cadence_decide(CadenceState& st, const CadenceParams& p,
                               bool has_noise, float noise_dba, uint32_t now_ms);
