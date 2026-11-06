#include "part.h"

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint32_t crc32_update(uint32_t c, const uint8_t* p, uint32_t n) {
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return c;
}
static uint32_t crc32(const uint8_t* p, uint32_t n) { return ~crc32_update(0xFFFFFFFFu, p, n); }

/* Загрузочный сектор FAT с нулевого сектора (без таблицы разделов). */
static int looks_fat(const uint8_t* b) {
    if ((b[0] != 0xEB && b[0] != 0xE9) || b[510] != 0x55 || b[511] != 0xAA) return 0;
    uint32_t bps = (uint32_t)(b[11] | b[12] << 8), spc = b[13];
    return bps == 512 && spc && !(spc & (spc - 1)) && b[16] >= 1 && b[16] <= 4 && (b[14] | b[15] << 8);
}

static int fat_type(uint8_t t) { return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E || t == 0x07; }

/* «Основные данные» Microsoft EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 в записи GPT (смешанный порядок байт). */
static const uint8_t basic_data[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };

static int scan_gpt(blk_dev_t* dev, part_t* out, int max) {
    static uint8_t h[512], e[512];
    if (!dev || !dev->read || dev->sectors < 2 || dev->read(dev, 1, 1, h) != 0) return 0;
    if (h[0] != 'E' || h[1] != 'F' || h[2] != 'I' || h[3] != ' ' || h[4] != 'P' || h[5] != 'A' || h[6] != 'R' || h[7] != 'T') return 0;
    uint32_t hsize = rd32(h + 12);
    if (hsize < 92 || hsize > 512) return 0;
    uint32_t want = rd32(h + 16); h[16] = h[17] = h[18] = h[19] = 0;
    if (crc32(h, hsize) != want) return 0;

    uint64_t current = rd64(h + 24), backup = rd64(h + 32);
    uint64_t first_usable = rd64(h + 40), last_usable = rd64(h + 48);
    uint64_t lba = rd64(h + 72);
    uint32_t n = rd32(h + 80), esz = rd32(h + 84), entries_crc = rd32(h + 88);
    if (current != 1 || backup <= current || backup >= dev->sectors ||
        first_usable < 2 || first_usable > last_usable || last_usable >= backup) return 0;
    if (esz < 128 || esz > 512 || (esz & (esz - 1)) || !n || n > 1024) return 0;

    uint64_t bytes = (uint64_t)n * esz;
    uint64_t sectors = (bytes + 511) / 512;
    if (lba < 2 || lba >= first_usable || sectors > first_usable - lba) return 0;

    uint32_t crc = 0xFFFFFFFFu;
    int found = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = (uint64_t)i * esz;
        uint64_t sector = off / 512;
        if ((off % 512) == 0) {
            if (dev->read(dev, lba + sector, 1, e) != 0) return 0;
            uint64_t remain = bytes - sector * 512;
            uint32_t chunk = remain > 512 ? 512 : (uint32_t)remain;
            crc = crc32_update(crc, e, chunk);
        }
        const uint8_t* p = e + off % 512;
        int same = 1; for (int k = 0; k < 16; k++) if (p[k] != basic_data[k]) same = 0;
        uint64_t first = rd64(p + 32), last = rd64(p + 40);
        if (!same || first < first_usable || last < first || last > last_usable || last >= dev->sectors) continue;
        if (found < max) out[found++] = (part_t){ first, last - first + 1, 0, 1 };
    }
    if (~crc != entries_crc) return 0;
    return found;
}

int part_scan(blk_dev_t* dev, const uint8_t* b, part_t* out, int max) {
    if (!dev || !dev->read || !b || !out || max <= 0 || b[510] != 0x55 || b[511] != 0xAA) return 0;
    int found = 0, protective = 0, valid_table = 1, any = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t* p = b + 446 + i * 16;
        if (p[0] != 0x00 && p[0] != 0x80) valid_table = 0;
        if (p[4]) any = 1;
        if (p[4] == 0xEE) protective = 1;
    }
    if (protective) { found = scan_gpt(dev, out, max); if (found) return found; }
    if (valid_table && any) {
        for (int i = 0; i < 4 && found < max; i++) {
            const uint8_t* p = b + 446 + i * 16;
            uint32_t start = rd32(p + 8), cnt = rd32(p + 12);
            if (!fat_type(p[4]) || !start || !cnt || (uint64_t)start + cnt > dev->sectors) continue;
            out[found++] = (part_t){ start, cnt, p[4], 0 };
        }
        if (found) return found;
    }
    if (looks_fat(b)) { out[0] = (part_t){ 0, dev->sectors, 0, 0 }; return 1; }
    return 0;
}
