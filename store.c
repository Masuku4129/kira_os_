#include "store.h"

static const char MAGIC[8] = {'K','I','R','A','E','X','P','1'};
#define VERSION 1

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t payload_len;
    uint64_t generation;
    uint32_t crc;
    uint32_t reserved;
    experience_t payload;
} __attribute__((packed)) record_t;

_Static_assert(sizeof(record_t) <= BLK_SECTOR, "record must fit one sector");

uint32_t crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return ~c;
}

static int valid(const uint8_t* sec, const record_t** out) {
    const record_t* r = (const record_t*)sec;
    for (int i = 0; i < 8; i++) if (r->magic[i] != MAGIC[i]) return 0;
    if (r->version != VERSION || r->payload_len != sizeof(experience_t)) return 0;
    uint8_t tmp[BLK_SECTOR];
    for (int i = 0; i < BLK_SECTOR; i++) tmp[i] = sec[i];
    ((record_t*)tmp)->crc = 0;
    if (crc32(tmp, BLK_SECTOR) != r->crc) return 0;
    *out = r;
    return 1;
}

int store_detect(blk_dev_t* dev) {
    uint8_t sec[BLK_SECTOR];
    const record_t* r;
    for (int i = 0; i < 2; i++) if (dev->read(dev, (uint64_t)i, 1, sec) == 0 && valid(sec, &r)) return 1;
    return 0;
}

int store_mount(store_t* s, blk_dev_t* dev, experience_t* out) {
    s->dev = dev;
    s->generation = 0;
    uint8_t sec[2][BLK_SECTOR];
    const record_t* best = 0;
    int have[2] = {0, 0};
    const record_t* rec[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        if (dev->read(dev, (uint64_t)i, 1, sec[i]) != 0) return -1;
        have[i] = valid(sec[i], &rec[i]);
    }
    if (have[0]) best = rec[0];
    if (have[1] && (!best || rec[1]->generation > best->generation)) best = rec[1];
    uint8_t* o = (uint8_t*)out;
    for (size_t i = 0; i < sizeof(*out); i++) o[i] = 0;
    if (!best) return 0;
    const uint8_t* src = (const uint8_t*)&best->payload;
    for (size_t i = 0; i < sizeof(*out); i++) o[i] = src[i];
    s->generation = best->generation;
    return 1;
}

int store_commit(store_t* s, const experience_t* e) {
    uint8_t sec[BLK_SECTOR];
    for (int i = 0; i < BLK_SECTOR; i++) sec[i] = 0;
    record_t* r = (record_t*)sec;
    for (int i = 0; i < 8; i++) r->magic[i] = MAGIC[i];
    r->version = VERSION;
    r->payload_len = sizeof(experience_t);
    uint64_t gen = s->generation + 1;
    r->generation = gen;
    const uint8_t* src = (const uint8_t*)e;
    uint8_t* dst = (uint8_t*)&r->payload;
    for (size_t i = 0; i < sizeof(*e); i++) dst[i] = src[i];
    r->crc = 0;
    r->crc = crc32(sec, BLK_SECTOR);

    if (s->dev->write(s->dev, gen & 1, 1, sec) != 0) return -1;
    if (s->dev->flush && s->dev->flush(s->dev) != 0) return -2;
    s->generation = gen;
    return 0;
}
