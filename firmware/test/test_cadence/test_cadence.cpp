#include <unity.h>
#include "cadence.h"

// Baseline params used across tests: densify on a 6 dB jump, hold 30 s, quiet
// baseline store every 60 s. The absolute trigger is parked above every level these
// delta-oriented tests use (55 dB) so they exercise the delta path in isolation.
static CadenceParams P() { return CadenceParams{ 6.0f, 30000, 60000, 55.0f }; }

void setUp() {}
void tearDown() {}

// First capture after boot is always stored, regardless of cadence.
void test_first_capture_always_stored() {
    CadenceState st;
    CadenceDecision d = cadence_decide(st, P(), true, 45.0f, 1000);
    TEST_ASSERT_TRUE(d.store);
    TEST_ASSERT_FALSE(d.densified);
}

// While quiet and steady, store only at the baseline interval.
void test_quiet_decimates_to_baseline() {
    CadenceState st;
    cadence_decide(st, P(), true, 45.0f, 0);              // primes + stores at t=0
    // 30 s later, still quiet -> not yet due
    TEST_ASSERT_FALSE(cadence_decide(st, P(), true, 45.2f, 30000).store);
    // 60 s after the last store -> due again
    TEST_ASSERT_TRUE(cadence_decide(st, P(), true, 45.1f, 60000).store);
}

// A sharp level jump arms densification: every capture stored during the window.
void test_jump_densifies_and_stores_every_capture() {
    CadenceState st;
    cadence_decide(st, P(), true, 45.0f, 0);             // prime quiet
    // +8 dB jump at t=5s -> densify
    CadenceDecision d = cadence_decide(st, P(), true, 53.0f, 5000);
    TEST_ASSERT_TRUE(d.densified);
    TEST_ASSERT_TRUE(d.store);
    // a closely-spaced capture (well under the 60 s baseline) still stores
    TEST_ASSERT_TRUE(cadence_decide(st, P(), true, 54.0f, 6300).store);
}

// Densification expires after the hold window once the level stops moving.
void test_densify_expires_after_hold() {
    CadenceState st;
    cadence_decide(st, P(), true, 45.0f, 0);
    cadence_decide(st, P(), true, 53.0f, 5000);          // densify until 35000
    TEST_ASSERT_TRUE(cadence_decide(st, P(), true, 53.5f, 30000).densified);
    // past the hold with no new jump -> quiet again
    TEST_ASSERT_FALSE(cadence_decide(st, P(), true, 53.6f, 40000).densified);
}

// A missing mic reading must not trigger or extend densification.
void test_missing_noise_skips_delta() {
    CadenceState st;
    cadence_decide(st, P(), true, 45.0f, 0);
    CadenceDecision d = cadence_decide(st, P(), false, 0.0f, 5000);
    TEST_ASSERT_FALSE(d.densified);
}

// A flyover-shaped slow ramp (~0.5 dB per 1.3 s capture) never trips the delta test,
// but must densify the moment it crosses the absolute threshold.
void test_slow_ramp_densifies_at_abs_threshold() {
    CadenceParams p{ 6.0f, 30000, 60000, 50.0f };
    CadenceState st;
    uint32_t t = 0;
    float level = 36.0f;
    CadenceDecision d{};
    while (level < 49.5f) {                     // approach: below threshold, tiny deltas
        d = cadence_decide(st, p, true, level, t);
        TEST_ASSERT_FALSE(d.densified);
        level += 0.5f; t += 1300;
    }
    d = cadence_decide(st, p, true, 50.0f, t);  // crosses the threshold
    TEST_ASSERT_TRUE(d.densified);
    TEST_ASSERT_TRUE(d.store);
}

// Each loud capture re-arms the hold, so densification rides the plateau and covers
// the decay tail for one hold window after the level drops back under the threshold.
void test_abs_plateau_rearms_then_expires() {
    CadenceParams p{ 6.0f, 30000, 60000, 50.0f };
    CadenceState st;
    cadence_decide(st, p, true, 36.0f, 0);                              // quiet prime
    for (uint32_t t = 1300; t <= 60000; t += 1300)                      // long 62 dB plateau
        TEST_ASSERT_TRUE(cadence_decide(st, p, true, 62.0f, t).densified);
    // drops to quiet (delta also re-arms once on the way down — that's fine);
    // 48 dB is under the threshold and within 6 dB of the previous quiet capture.
    cadence_decide(st, p, true, 48.0f, 61300);
    TEST_ASSERT_TRUE(cadence_decide(st, p, true, 47.0f, 70000).densified);   // tail covered
    TEST_ASSERT_FALSE(cadence_decide(st, p, true, 47.0f, 130000).densified); // hold expired
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_capture_always_stored);
    RUN_TEST(test_quiet_decimates_to_baseline);
    RUN_TEST(test_jump_densifies_and_stores_every_capture);
    RUN_TEST(test_densify_expires_after_hold);
    RUN_TEST(test_missing_noise_skips_delta);
    RUN_TEST(test_slow_ramp_densifies_at_abs_threshold);
    RUN_TEST(test_abs_plateau_rearms_then_expires);
    return UNITY_END();
}
