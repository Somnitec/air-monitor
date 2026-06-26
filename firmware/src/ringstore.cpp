#include "ringstore.h"
#include "ringlogic.h"
#include "config.h"
#include <LittleFS.h>
#include <Arduino.h>

static RingLogic s_logic;

// On-flash metadata, written to two alternating slots in /ring.meta.
struct Meta {
    uint32_t magic;        // 'A''R''N''G'
    uint8_t  schema;       // RECORD_SCHEMA_VERSION
    uint8_t  rec_size;     // RECORD_SIZE
    uint16_t _pad;
    uint32_t capacity;
    uint32_t head_seq;
    uint32_t tail_seq;
    uint32_t synced_seq;
    uint32_t write_counter;
    uint32_t crc;          // crc32 over all preceding bytes
};
static constexpr uint32_t META_MAGIC = 0x41524E47;  // "ARNG"
static uint32_t s_writeCounter = 0;

static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320 & (-(int)(c & 1)));
    }
    return ~c;
}

static bool metaValid(const Meta& m) {
    if (m.magic != META_MAGIC) return false;
    uint32_t want = crc32((const uint8_t*)&m, sizeof(Meta) - sizeof(uint32_t));
    return want == m.crc && m.capacity == RING_CAPACITY &&
           m.rec_size == RECORD_SIZE && m.schema == RECORD_SCHEMA_VERSION;
}

static void persistMeta() {
    Meta m{};
    m.magic = META_MAGIC; m.schema = RECORD_SCHEMA_VERSION; m.rec_size = RECORD_SIZE;
    m.capacity = s_logic.capacity;
    m.head_seq = s_logic.head_seq; m.tail_seq = s_logic.tail_seq; m.synced_seq = s_logic.synced_seq;
    m.write_counter = ++s_writeCounter;
    m.crc = crc32((const uint8_t*)&m, sizeof(Meta) - sizeof(uint32_t));

    // alternate between slot 0 and 1 so a torn write never destroys both copies
    size_t off = (s_writeCounter & 1) ? sizeof(Meta) : 0;
    File f = LittleFS.open(RING_META_PATH, "r+");
    if (!f) f = LittleFS.open(RING_META_PATH, "w+");
    if (!f) { Serial.println("[ring] meta open failed"); return; }
    f.seek(off);
    f.write((const uint8_t*)&m, sizeof(Meta));
    f.close();
}

static bool loadMeta() {
    File f = LittleFS.open(RING_META_PATH, "r");
    if (!f) return false;
    Meta a{}, b{};
    f.read((uint8_t*)&a, sizeof(Meta));
    f.read((uint8_t*)&b, sizeof(Meta));
    f.close();
    bool va = metaValid(a), vb = metaValid(b);
    const Meta* pick = nullptr;
    if (va && vb) pick = (a.write_counter >= b.write_counter) ? &a : &b;
    else if (va)  pick = &a;
    else if (vb)  pick = &b;
    if (!pick) return false;
    s_logic.capacity = pick->capacity;
    s_logic.head_seq = pick->head_seq; s_logic.tail_seq = pick->tail_seq; s_logic.synced_seq = pick->synced_seq;
    s_writeCounter = pick->write_counter;
    return true;
}

static bool preallocate() {
    File f = LittleFS.open(RING_PATH, "r");
    size_t want = (size_t)RING_CAPACITY * RECORD_SIZE;
    if (f && f.size() == want) { f.close(); return true; }
    if (f) f.close();
    f = LittleFS.open(RING_PATH, "w");
    if (!f) return false;
    uint8_t zero[256] = {0};
    size_t left = want;
    while (left) { size_t n = left < sizeof(zero) ? left : sizeof(zero); f.write(zero, n); left -= n; }
    f.close();
    Serial.printf("[ring] preallocated %u bytes\n", (unsigned)want);
    return true;
}

bool ringstore_begin() {
    if (LittleFS.exists(LEGACY_QUEUE_PATH))  LittleFS.remove(LEGACY_QUEUE_PATH);
    if (LittleFS.exists(LEGACY_CURSOR_PATH)) LittleFS.remove(LEGACY_CURSOR_PATH);

    ring_init(s_logic, RING_CAPACITY);
    if (!preallocate()) { Serial.println("[ring] preallocate failed"); return false; }
    if (!loadMeta()) {
        Serial.println("[ring] no valid meta — starting fresh");
        persistMeta();
    } else {
        Serial.printf("[ring] resumed head=%u tail=%u synced=%u\n",
                      s_logic.head_seq, s_logic.tail_seq, s_logic.synced_seq);
    }
    return true;
}

bool ringstore_push(const Record& rec) {
    uint32_t slot = ring_push_slot(s_logic);
    File f = LittleFS.open(RING_PATH, "r+");
    if (!f) { Serial.println("[ring] push open failed"); return false; }
    f.seek((size_t)slot * RECORD_SIZE);
    Record r = rec; r.seq = s_logic.head_seq - 1;     // stamp the assigned seq
    size_t w = f.write((const uint8_t*)&r, RECORD_SIZE);
    f.close();
    if (w != RECORD_SIZE) { Serial.println("[ring] short write"); return false; }
    persistMeta();                                     // data first, then meta
    return true;
}

uint32_t ringstore_drain(Record* out, uint32_t maxN, uint32_t* lastSeq) {
    uint32_t slots[SYNC_BATCH_MAX], seqs[SYNC_BATCH_MAX];
    if (maxN > SYNC_BATCH_MAX) maxN = SYNC_BATCH_MAX;
    uint32_t n = ring_drain_slots(s_logic, maxN, slots, seqs);
    if (n == 0) return 0;
    File f = LittleFS.open(RING_PATH, "r");
    if (!f) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        f.seek((size_t)slots[i] * RECORD_SIZE);
        f.read((uint8_t*)&out[i], RECORD_SIZE);
    }
    f.close();
    if (lastSeq) *lastSeq = seqs[n - 1];
    return n;
}

void ringstore_mark_synced(uint32_t lastSeq) {
    ring_mark_synced(s_logic, lastSeq);
    persistMeta();
}

uint32_t ringstore_count(void)    { return ring_count(s_logic); }
uint32_t ringstore_unsynced(void) { return ring_unsynced(s_logic); }
