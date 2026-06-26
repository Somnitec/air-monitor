#include <unity.h>
#include "ringlogic.h"

void setUp() {}
void tearDown() {}

void test_push_advances_head_and_slot_wraps() {
    RingLogic r; ring_init(r, 4);
    TEST_ASSERT_EQUAL_UINT32(0, ring_push_slot(r));   // seq0 -> slot0
    TEST_ASSERT_EQUAL_UINT32(1, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(2, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(3, ring_push_slot(r));
    TEST_ASSERT_EQUAL_UINT32(0, ring_push_slot(r));   // seq4 -> slot0 (wrap)
    TEST_ASSERT_EQUAL_UINT32(4, ring_count(r));         // still 4 (capacity)
}

void test_overflow_drops_oldest() {
    RingLogic r; ring_init(r, 3);
    for (int i = 0; i < 5; ++i) ring_push_slot(r);    // push seq 0..4
    TEST_ASSERT_EQUAL_UINT32(3, ring_count(r));
    TEST_ASSERT_EQUAL_UINT32(2, r.tail_seq);            // oldest kept is seq2
    TEST_ASSERT_EQUAL_UINT32(5, r.head_seq);
}

void test_drain_and_mark_synced() {
    RingLogic r; ring_init(r, 8);
    for (int i = 0; i < 5; ++i) ring_push_slot(r);    // seq 0..4
    uint32_t slots[8], seqs[8];
    uint32_t n = ring_drain_slots(r, 8, slots, seqs);
    TEST_ASSERT_EQUAL_UINT32(5, n);
    TEST_ASSERT_EQUAL_UINT32(0, seqs[0]);
    TEST_ASSERT_EQUAL_UINT32(4, seqs[4]);

    ring_mark_synced(r, 2);                            // acked through seq2 (3 records: 0,1,2)
    TEST_ASSERT_EQUAL_UINT32(3, r.synced_seq);          // synced_seq is exclusive upper bound
    TEST_ASSERT_EQUAL_UINT32(2, ring_unsynced(r));      // seq3,4 remain
    n = ring_drain_slots(r, 8, slots, seqs);
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT32(3, seqs[0]);
}

void test_overflow_past_unsynced_pulls_synced_forward() {
    RingLogic r; ring_init(r, 3);
    for (int i = 0; i < 3; ++i) ring_push_slot(r);    // seq 0,1,2 ; none synced
    ring_push_slot(r);                                 // seq3 overwrites seq0 (unsynced lost)
    TEST_ASSERT_EQUAL_UINT32(1, r.tail_seq);
    TEST_ASSERT_TRUE(r.synced_seq >= r.tail_seq);       // invariant holds
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_push_advances_head_and_slot_wraps);
    RUN_TEST(test_overflow_drops_oldest);
    RUN_TEST(test_drain_and_mark_synced);
    RUN_TEST(test_overflow_past_unsynced_pulls_synced_forward);
    return UNITY_END();
}
