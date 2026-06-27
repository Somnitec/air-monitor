#include "ringstore.h"
#include "seglogic.h"
#include "config.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <string.h>

// =========================================================================
// Segment-log FIFO on LittleFS.
//
// Why not one big preallocated file with in-place slot writes?  LittleFS stores
// each file as a CTZ skip-list; overwriting a block in the *middle* of a large
// file copies-on-write that block AND every block after it to EOF.  A random
// write to an 852 KB ring therefore needs ~the whole file's worth of free
// blocks at once — which both crashes the allocator (genuine NOSPC, surfaced as
// an IntegerDivideByZero inside esp_littlefs) and would shred the flash.
//
// Instead the queue is a set of append-only segment files under QUEUE_DIR:
//   /q/<8-hex-index>.seg   each holds up to SEG_RECORDS packed Records.
// Appends touch only the tail block (cheap on a CTZ list); dropping the oldest
// data is a whole-file remove (frees a segment's blocks at once, no rewrite).
// A record's global seq maps implicitly to (segment, offset):
//   segment = seq / SEG_RECORDS,  byte_offset = (seq % SEG_RECORDS) * RECORD_SIZE
// so head/tail are recovered on boot by scanning the directory; only the synced
// cursor needs persisting (double-buffered + CRC in /q/cursor).
// =========================================================================

static constexpr uint32_t MAX_SEGMENTS = (RING_CAPACITY + SEG_RECORDS - 1) / SEG_RECORDS;

static uint32_t s_headSeq   = 0;   // next seq to write
static uint32_t s_tailSeq   = 0;   // oldest retained seq (always a SEG_RECORDS multiple)
static uint32_t s_syncedSeq = 0;   // acked boundary (tail <= synced <= head)
static bool     s_ok        = false;

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320 & (-(int)(c & 1)));
    }
    return ~c;
}

static String segPath(uint32_t seg) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%s/%08lx.seg", QUEUE_DIR, (unsigned long)seg);
    return String(buf);
}

// Parse a "<hex>.seg" leaf name into its segment index. Accepts a full path too.
static bool parseSeg(const char* nm, uint32_t& idx) {
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    size_t len = strlen(nm);
    if (len < 5 || strcmp(nm + len - 4, ".seg") != 0) return false;
    char* end = nullptr;
    unsigned long v = strtoul(nm, &end, 16);
    if (end != nm + (len - 4)) return false;   // entire prefix must be hex
    idx = (uint32_t)v;
    return true;
}

// ---- synced cursor: double-buffered, CRC'd ----
struct Cursor { uint32_t magic; uint32_t synced_seq; uint32_t counter; uint32_t crc; };
static constexpr uint32_t CURSOR_MAGIC = 0x51435552;   // "QCUR"
static uint32_t s_cursorCtr = 0;

static bool cursorValid(const Cursor& c) {
    return c.magic == CURSOR_MAGIC &&
           crc32((const uint8_t*)&c, sizeof(Cursor) - sizeof(uint32_t)) == c.crc;
}

static void persistCursor() {
    Cursor c{};
    c.magic = CURSOR_MAGIC; c.synced_seq = s_syncedSeq; c.counter = ++s_cursorCtr;
    c.crc = crc32((const uint8_t*)&c, sizeof(Cursor) - sizeof(uint32_t));
    size_t off = (s_cursorCtr & 1) ? sizeof(Cursor) : 0;   // alternate slots
    File f = LittleFS.open(QUEUE_CURSOR_PATH, "r+");
    if (!f) f = LittleFS.open(QUEUE_CURSOR_PATH, "w+");
    if (!f) { Serial.println("[ring] cursor open failed"); return; }
    f.seek(off);
    f.write((const uint8_t*)&c, sizeof(Cursor));
    f.close();
}

static bool loadCursor(uint32_t& out) {
    File f = LittleFS.open(QUEUE_CURSOR_PATH, "r");
    if (!f) return false;
    Cursor a{}, b{};
    f.read((uint8_t*)&a, sizeof(Cursor));
    f.read((uint8_t*)&b, sizeof(Cursor));
    f.close();
    bool va = cursorValid(a), vb = cursorValid(b);
    const Cursor* pick = nullptr;
    if (va && vb) pick = (a.counter >= b.counter) ? &a : &b;
    else if (va)  pick = &a;
    else if (vb)  pick = &b;
    if (!pick) return false;
    s_cursorCtr = pick->counter;
    out = pick->synced_seq;
    return true;
}

bool ringstore_begin() {
    s_ok = false;
    // Reclaim space from any earlier firmware revision's queue files.
    const char* legacy[] = { LEGACY_QUEUE_PATH, LEGACY_CURSOR_PATH, RING_PATH, RING_META_PATH };
    for (const char* p : legacy) if (LittleFS.exists(p)) LittleFS.remove(p);

    if (!LittleFS.exists(QUEUE_DIR) && !LittleFS.mkdir(QUEUE_DIR)) {
        Serial.println("[ring] mkdir failed");
        return false;
    }

    // Recover head/tail by scanning the segment directory.
    bool any = false;
    uint32_t minSeg = 0, maxSeg = 0;
    File dir = LittleFS.open(QUEUE_DIR);
    if (dir && dir.isDirectory()) {
        for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
            uint32_t idx;
            if (parseSeg(e.name(), idx)) {
                if (!any) { minSeg = maxSeg = idx; any = true; }
                else { if (idx < minSeg) minSeg = idx; if (idx > maxSeg) maxSeg = idx; }
            }
            e.close();
        }
    }
    if (dir) dir.close();

    if (!any) {
        s_headSeq = s_tailSeq = s_syncedSeq = 0;
    } else {
        File hf = LittleFS.open(segPath(maxSeg), "r");
        uint32_t headCount = hf ? (uint32_t)(hf.size() / RECORD_SIZE) : 0;
        if (hf) hf.close();
        seg_recover(minSeg, maxSeg, headCount, SEG_RECORDS, s_headSeq, s_tailSeq);
        s_syncedSeq = s_tailSeq;
    }

    uint32_t cur;                                // restore durable synced boundary
    if (loadCursor(cur)) s_syncedSeq = seg_clamp_synced(cur, s_tailSeq, s_headSeq);

    s_ok = true;
    Serial.printf("[ring] %s head=%lu tail=%lu synced=%lu (%u stored, %u unsynced)\n",
                  any ? "resumed" : "fresh",
                  (unsigned long)s_headSeq, (unsigned long)s_tailSeq, (unsigned long)s_syncedSeq,
                  (unsigned)ringstore_count(), (unsigned)ringstore_unsynced());
    return true;
}

bool ringstore_push(const Record& rec) {
    if (!s_ok) return false;
    uint32_t seq = s_headSeq;
    Record r = rec; r.seq = seq;                 // stamp the assigned seq

    File f = LittleFS.open(segPath(seg_index_of(seq, SEG_RECORDS)), "a");
    if (!f) { Serial.println("[ring] seg open failed"); return false; }
    size_t w = f.write((const uint8_t*)&r, RECORD_SIZE);
    f.close();
    if (w != RECORD_SIZE) { Serial.println("[ring] short write"); return false; }

    s_headSeq++;
    // Drop-oldest: remove whole segments once we exceed the retention window.
    uint32_t firstSeg = seg_index_of(s_tailSeq, SEG_RECORDS);
    uint32_t oldSynced = s_syncedSeq;
    uint32_t drop = seg_evict(s_headSeq, s_tailSeq, s_syncedSeq, SEG_RECORDS, MAX_SEGMENTS);
    for (uint32_t i = 0; i < drop; ++i) LittleFS.remove(segPath(firstSeg + i));
    if (s_syncedSeq != oldSynced) persistCursor();
    return true;
}

uint32_t ringstore_drain(Record* out, uint32_t maxN, uint32_t* lastSeq) {
    if (!s_ok) return 0;
    uint32_t n = 0, openSeg = 0xFFFFFFFF;
    File f;
    for (uint32_t seq = s_syncedSeq; seq < s_headSeq && n < maxN; ++seq) {
        uint32_t seg = seg_index_of(seq, SEG_RECORDS);
        if (seg != openSeg) {
            if (f) f.close();
            f = LittleFS.open(segPath(seg), "r");
            openSeg = seg;
            if (!f) break;                       // missing segment — stop here
        }
        if (!f.seek(seg_offset_of(seq, SEG_RECORDS, RECORD_SIZE))) break;
        if (f.read((uint8_t*)&out[n], RECORD_SIZE) != (int)RECORD_SIZE) break;
        if (lastSeq) *lastSeq = out[n].seq;
        ++n;
    }
    if (f) f.close();
    return n;
}

void ringstore_mark_synced(uint32_t lastSeq) {
    if (!s_ok) return;
    uint32_t next = lastSeq + 1;
    if (next > s_headSeq) next = s_headSeq;
    if (next > s_syncedSeq) { s_syncedSeq = next; persistCursor(); }
}

uint32_t ringstore_count(void)    { return s_headSeq - s_tailSeq; }
uint32_t ringstore_unsynced(void) { return s_headSeq - s_syncedSeq; }
