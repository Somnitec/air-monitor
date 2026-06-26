#include "ringlogic.h"

void ring_init(RingLogic& r, uint32_t capacity) {
    r.capacity = capacity;
    r.head_seq = r.tail_seq = r.synced_seq = 0;
}

uint32_t ring_push_slot(RingLogic& r) {
    uint32_t slot = r.head_seq % r.capacity;
    r.head_seq++;
    if (r.head_seq - r.tail_seq > r.capacity) {
        r.tail_seq = r.head_seq - r.capacity;          // drop oldest
        if (r.synced_seq < r.tail_seq) r.synced_seq = r.tail_seq;
    }
    return slot;
}

uint32_t ring_count(const RingLogic& r)    { return r.head_seq - r.tail_seq; }
uint32_t ring_unsynced(const RingLogic& r) { return r.head_seq - r.synced_seq; }

uint32_t ring_drain_slots(const RingLogic& r, uint32_t maxN,
                          uint32_t* outSlots, uint32_t* outSeqs) {
    uint32_t n = 0;
    for (uint32_t seq = r.synced_seq; seq < r.head_seq && n < maxN; ++seq, ++n) {
        outSlots[n] = seq % r.capacity;
        outSeqs[n]  = seq;
    }
    return n;
}

void ring_mark_synced(RingLogic& r, uint32_t throughSeq) {
    uint32_t next = throughSeq + 1;
    if (next > r.head_seq) next = r.head_seq;
    if (next > r.synced_seq) r.synced_seq = next;
}
