#include <unity.h>
#include "cadence.h"

// Baseline params used across tests: densify on a 6 dB jump, hold 30 s, quiet
// baseline store every 60 s.
static CadenceParams P() { return CadenceParams{ 6.0f, 30000, 60000 }; }

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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_capture_always_stored);
    RUN_TEST(test_quiet_decimates_to_baseline);
    RUN_TEST(test_jump_densifies_and_stores_every_capture);
    RUN_TEST(test_densify_expires_after_hold);
    RUN_TEST(test_missing_noise_skips_delta);
    return UNITY_END();
}
