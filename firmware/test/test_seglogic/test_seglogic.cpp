#include <unity.h>
#include "seglogic.h"

// Mirror the production config (config.h SEG_RECORDS / RING_CAPACITY, record.h
// RECORD_SIZE) but keep the test self-contained and Arduino-free.
static const uint32_t SR   = 500;   // SEG_RECORDS
static const uint32_t RS   = 71;    // RECORD_SIZE
static const uint32_t MAXS = 24;    // ceil(12000 / 500)

void setUp() {}
void tearDown() {}

void test_index_and_offset_mapping() {
    TEST_ASSERT_EQUAL_UINT32(0,  seg_index_of(0,        SR));
    TEST_ASSERT_EQUAL_UINT32(0,  seg_index_of(SR - 1,   SR));
    TEST_ASSERT_EQUAL_UINT32(1,  seg_index_of(SR,       SR));    // boundary rolls
    TEST_ASSERT_EQUAL_UINT32(42, seg_index_of(42 * SR + 3, SR));

    TEST_ASSERT_EQUAL_UINT32(0,            seg_offset_of(0,          SR, RS));
    TEST_ASSERT_EQUAL_UINT32(RS,           seg_offset_of(1,          SR, RS));
    TEST_ASSERT_EQUAL_UINT32((SR - 1) * RS, seg_offset_of(SR - 1,    SR, RS));
    TEST_ASSERT_EQUAL_UINT32(3 * RS,       seg_offset_of(7 * SR + 3, SR, RS)); // wraps per segment
}

void test_recover_fresh_partial_and_full_head() {
    uint32_t h, t;
    // single partial head segment (index 0) holding 10 records
    seg_recover(0, 0, 10, SR, h, t);
    TEST_ASSERT_EQUAL_UINT32(0,  t);
    TEST_ASSERT_EQUAL_UINT32(10, h);

    // segments 3..7 present, head segment (7) holds 250 records
    seg_recover(3, 7, 250, SR, h, t);
    TEST_ASSERT_EQUAL_UINT32(3 * SR,       t);
    TEST_ASSERT_EQUAL_UINT32(7 * SR + 250, h);

    // a full head segment: next push must roll to the following segment
    seg_recover(0, 0, SR, SR, h, t);
    TEST_ASSERT_EQUAL_UINT32(SR, h);
    TEST_ASSERT_EQUAL_UINT32(1,  seg_index_of(h, SR));
}

void test_evict_noop_until_window_exceeded() {
    uint32_t tail = 0, synced = 0;
    uint32_t retain = MAXS * SR;
    // exactly at the window — nothing dropped
    TEST_ASSERT_EQUAL_UINT32(0, seg_evict(retain, tail, synced, SR, MAXS));
    TEST_ASSERT_EQUAL_UINT32(0, tail);
    // one record over — drop exactly one whole segment
    TEST_ASSERT_EQUAL_UINT32(1, seg_evict(retain + 1, tail, synced, SR, MAXS));
    TEST_ASSERT_EQUAL_UINT32(SR, tail);
}

void test_evict_drops_multiple_segments_at_once() {
    uint32_t tail = 0, synced = 0;
    // jump far past the window (e.g. after restoring a huge head) → drop several
    uint32_t head = (MAXS + 3) * SR;
    uint32_t drop = seg_evict(head, tail, synced, SR, MAXS);
    TEST_ASSERT_EQUAL_UINT32(3, drop);
    TEST_ASSERT_EQUAL_UINT32(3 * SR, tail);
    TEST_ASSERT_TRUE(head - tail <= MAXS * SR);
}

void test_evict_pulls_synced_forward_when_overtaken() {
    uint32_t tail = 0, synced = 0;          // nothing synced yet
    uint32_t drop = seg_evict(MAXS * SR + SR, tail, synced, SR, MAXS);
    TEST_ASSERT_EQUAL_UINT32(1,  drop);
    TEST_ASSERT_EQUAL_UINT32(SR, tail);
    TEST_ASSERT_EQUAL_UINT32(SR, synced);    // synced can't lag behind tail
    TEST_ASSERT_TRUE(synced >= tail);
}

void test_evict_keeps_synced_when_ahead() {
    uint32_t tail = 0, synced = 5 * SR;     // already synced past the dropped segment
    seg_evict(MAXS * SR + SR, tail, synced, SR, MAXS);
    TEST_ASSERT_EQUAL_UINT32(SR,     tail);
    TEST_ASSERT_EQUAL_UINT32(5 * SR, synced); // untouched — eviction didn't overtake it
}

void test_clamp_synced_into_window() {
    TEST_ASSERT_EQUAL_UINT32(100, seg_clamp_synced(50,  100, 200)); // below tail -> tail
    TEST_ASSERT_EQUAL_UINT32(200, seg_clamp_synced(250, 100, 200)); // above head -> head
    TEST_ASSERT_EQUAL_UINT32(150, seg_clamp_synced(150, 100, 200)); // in range -> unchanged
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_index_and_offset_mapping);
    RUN_TEST(test_recover_fresh_partial_and_full_head);
    RUN_TEST(test_evict_noop_until_window_exceeded);
    RUN_TEST(test_evict_drops_multiple_segments_at_once);
    RUN_TEST(test_evict_pulls_synced_forward_when_overtaken);
    RUN_TEST(test_evict_keeps_synced_when_ahead);
    RUN_TEST(test_clamp_synced_into_window);
    return UNITY_END();
}
