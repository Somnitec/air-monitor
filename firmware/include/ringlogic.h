#pragma once
// Pure FIFO ring pointer arithmetic — no IO, fully host-testable. ringstore
// binds this to a LittleFS file. Sequence numbers are monotonic; a record's
// slot is seq % capacity. Drop-oldest on overflow.

#include <stdint.h>

struct RingLogic {
    uint32_t capacity   = 0;
    uint32_t head_seq   = 0;   // next seq to write
    uint32_t tail_seq   = 0;   // oldest seq still stored
    uint32_t synced_seq = 0;   // acked boundary (tail <= synced <= head)
};

void     ring_init(RingLogic& r, uint32_t capacity);

// Reserve the next write slot. Advances head; on overflow advances tail
// (drop-oldest) and synced if it was overtaken. Returns the slot index to write.
uint32_t ring_push_slot(RingLogic& r);

uint32_t ring_count(const RingLogic& r);      // head - tail
uint32_t ring_unsynced(const RingLogic& r);   // head - synced

// Fill up to maxN slot indices + seqs for the unsynced span [synced, head).
// Returns how many were filled.
uint32_t ring_drain_slots(const RingLogic& r, uint32_t maxN,
                          uint32_t* outSlots, uint32_t* outSeqs);

// Advance synced up to (and including) throughSeq, clamped to [synced, head].
void     ring_mark_synced(RingLogic& r, uint32_t throughSeq);
