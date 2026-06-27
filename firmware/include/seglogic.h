#pragma once
// Pure arithmetic for the append-only segment queue (see ringstore.cpp). No IO,
// fully host-testable — mirrors the ringlogic/ringstore split. A record's global
// seq maps implicitly to (segment index, byte offset within the segment); the
// queue retains at most max_segments whole segments and evicts the oldest in
// segment-sized chunks (drop-oldest).

#include <stdint.h>

// Segment index holding `seq`, and the byte offset of that record within it.
uint32_t seg_index_of (uint32_t seq, uint32_t seg_records);
uint32_t seg_offset_of(uint32_t seq, uint32_t seg_records, uint32_t rec_size);

// Reconstruct head/tail seq from a directory scan: the lowest and highest
// segment indices present, plus how many records the highest (head) segment
// holds. (Caller uses head=tail=0 when no segments exist.)
void seg_recover(uint32_t min_seg, uint32_t max_seg, uint32_t head_count,
                 uint32_t seg_records, uint32_t& head_seq, uint32_t& tail_seq);

// Evict oldest whole segments until head-tail <= max_segments*seg_records.
// Advances `tail_seq` (and pulls `synced_seq` up if it was overtaken). Returns
// how many segments to delete from disk, starting at the pre-call tail segment.
uint32_t seg_evict(uint32_t head_seq, uint32_t& tail_seq, uint32_t& synced_seq,
                   uint32_t seg_records, uint32_t max_segments);

// Clamp a restored synced cursor into the valid [tail, head] window.
uint32_t seg_clamp_synced(uint32_t synced, uint32_t tail_seq, uint32_t head_seq);
