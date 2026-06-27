#pragma once
// LittleFS-backed binary FIFO queue, stored as append-only segment files under
// QUEUE_DIR (see ringstore.cpp for why a single big file is unusable on
// LittleFS). A record's global seq maps implicitly to (segment, offset), so
// head/tail are recovered by scanning the directory; only the synced cursor is
// persisted (double-buffered + CRC). Worst-case loss on a power cut is the
// single in-flight record (data is appended before the cursor advances).

#include "record.h"
#include <stdint.h>

// Mount the queue: delete legacy files, ensure QUEUE_DIR exists, recover
// head/tail/synced from the segment files + cursor. Call once in setup() AFTER
// LittleFS.begin(). Returns false on unrecoverable FS error.
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
