#include "seglogic.h"

uint32_t seg_index_of(uint32_t seq, uint32_t seg_records) {
    return seq / seg_records;
}

uint32_t seg_offset_of(uint32_t seq, uint32_t seg_records, uint32_t rec_size) {
    return (seq % seg_records) * rec_size;
}

void seg_recover(uint32_t min_seg, uint32_t max_seg, uint32_t head_count,
                 uint32_t seg_records, uint32_t& head_seq, uint32_t& tail_seq) {
    tail_seq = min_seg * seg_records;
    head_seq = max_seg * seg_records + head_count;
}

uint32_t seg_evict(uint32_t head_seq, uint32_t& tail_seq, uint32_t& synced_seq,
                   uint32_t seg_records, uint32_t max_segments) {
    uint32_t retain = max_segments * seg_records;
    uint32_t dropped = 0;
    while (head_seq - tail_seq > retain) {
        tail_seq += seg_records;
        if (synced_seq < tail_seq) synced_seq = tail_seq;   // unsynced data lost
        ++dropped;
    }
    return dropped;
}

uint32_t seg_clamp_synced(uint32_t synced, uint32_t tail_seq, uint32_t head_seq) {
    if (synced < tail_seq) return tail_seq;
    if (synced > head_seq) return head_seq;
    return synced;
}
