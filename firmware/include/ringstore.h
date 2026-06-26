#pragma once
// LittleFS-backed binary FIFO ring. Binds ringlogic (pointer math) to a
// preallocated /ring.bin of RING_CAPACITY fixed slots, with pointers persisted
// in a double-buffered CRC'd /ring.meta. Worst-case loss on a power cut is the
// single in-flight record (data is written before metadata).

#include "record.h"
#include <stdint.h>

// Mount LittleFS-resident state: load+validate meta (or init), preallocate
// /ring.bin if absent, and delete any legacy NDJSON queue files. Call once in
// setup() AFTER LittleFS.begin(). Returns false on unrecoverable FS error.
bool ringstore_begin();

// Append one record (drop-oldest if full). Returns false on write error.
bool ringstore_push(const Record& rec);

// Read up to maxN un-synced records (oldest first) into out[]; also returns the
// seq of the last one in *lastSeq. Returns the count read.
uint32_t ringstore_drain(Record* out, uint32_t maxN, uint32_t* lastSeq);

// Mark everything through lastSeq as synced and persist the new pointer.
void ringstore_mark_synced(uint32_t lastSeq);

uint32_t ringstore_count(void);      // total stored
uint32_t ringstore_unsynced(void);   // not yet acked
