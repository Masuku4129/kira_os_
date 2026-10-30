#include "fat.h"

#define FAT_RUN_MAX 256u                   /* секторов в одном запросе к устройству (128 КБ) */

/* ---------------- служебное ---------------- */

static void mset(void* d, int c, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = (uint8_t)c; }
static void mcpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void default_clock(uint16_t* d, uint16_t* t) { *d = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1); *t = 0; }
static void (*clock_fn)(uint16_t*, uint16_t*) = default_clock;
void fat_set_clock(void (*now)(uint16_t*, uint16_t*)) { clock_fn = now ? now : default_clock; }

#define ATTR_RO 0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYS 0x04
#define ATTR_VOL 0x08
#define ATTR_DIR 0x10
#define ATTR_ARCH 0x20
#define ATTR_LFN 0x0F

/* ---------------- секторы ---------------- */

static int dev_read(fat_vol_t* v, uint64_t rel, uint32_t n, void* buf) {
    if (v->dev->read(v->dev, v->base + rel, n, buf) != 0) { v->io_errors++; return KFS_ERR_IO; }
    return 0;
}
static int dev_write(fat_vol_t* v, uint64_t rel, uint32_t n, const void* buf) {
    if (v->dev->write(v->dev, v->base + rel, n, buf) != 0) { v->io_errors++; return KFS_ERR_IO; }
    return 0;
}
static int sec_load(fat_vol_t* v, uint64_t rel) {
    if (v->sec_valid && v->sec_lba == rel) return 0;
    v->sec_valid = 0;
    int r = dev_read(v, rel, 1, v->sec);
    if (r) return r;
    v->sec_lba = rel; v->sec_valid = 1;
    return 0;
}
static int sec_store(fat_vol_t* v) { return dev_write(v, v->sec_lba, 1, v->sec); }

/* ---------------- таблица FAT ---------------- */

static int fsec_flush(fat_vol_t* v) {
    if (!v->fsec_valid || !v->fsec_dirty) return 0;
    for (uint32_t k = 0; k < v->nfats; k++) {                       /* все копии таблицы */
        int r = dev_write(v, v->rsvd + k * v->fatsz + v->fsec_no, 1, v->fsec);
        if (r) return r;
    }
    v->fsec_dirty = 0;
    return 0;
}
static int fsec_load(fat_vol_t* v, uint32_t no) {
    if (v->fsec_valid && v->fsec_no == no) return 0;
    int r = fsec_flush(v); if (r) return r;
    v->fsec_valid = 0;
    r = dev_read(v, v->rsvd + no, 1, v->fsec); if (r) return r;
    v->fsec_no = no; v->fsec_valid = 1; v->fsec_dirty = 0;
    return 0;
}

/* Байт номер off таблицы (FAT12 может пересекать границу сектора - по байту). */
static int fat_byte(fat_vol_t* v, uint32_t off, uint8_t** p) {
    int r = fsec_load(v, off / FAT_SECTOR); if (r) return r;
    *p = &v->fsec[off % FAT_SECTOR];
    return 0;
}

static int fat_get(fat_vol_t* v, uint32_t c, uint32_t* out) {
    if (c >= v->clusters + 2) return KFS_ERR_CORRUPT;
    uint8_t* p; int r;
    if (v->type == 32) { if ((r = fat_byte(v, c * 4, &p))) return r; *out = rd32(p) & 0x0FFFFFFF; return 0; }
    if (v->type == 16) { if ((r = fat_byte(v, c * 2, &p))) return r; *out = rd16(p); return 0; }
    uint32_t off = c + c / 2; uint8_t lo, hi;
    if ((r = fat_byte(v, off, &p))) return r;
    lo = *p;
    if ((r = fat_byte(v, off + 1, &p))) return r;
    hi = *p;
    uint16_t w = (uint16_t)(lo | hi << 8);
    *out = (c & 1) ? (uint32_t)(w >> 4) : (uint32_t)(w & 0xFFF);
    return 0;
}

static int fat_set(fat_vol_t* v, uint32_t c, uint32_t val) {
    if (c < 2 || c >= v->clusters + 2) return KFS_ERR_CORRUPT;
    uint8_t* p; int r;
    if (v->type == 32) {
        if ((r = fat_byte(v, c * 4, &p))) return r;
        wr32(p, (rd32(p) & 0xF0000000u) | (val & 0x0FFFFFFF)); v->fsec_dirty = 1; return 0;
    }
    if (v->type == 16) { if ((r = fat_byte(v, c * 2, &p))) return r; wr16(p, (uint16_t)val); v->fsec_dirty = 1; return 0; }
    uint32_t off = c + c / 2;
    if ((r = fat_byte(v, off, &p))) return r;
    if (c & 1) *p = (uint8_t)((*p & 0x0F) | ((val << 4) & 0xF0)); else *p = (uint8_t)val;
    v->fsec_dirty = 1;
    if ((r = fat_byte(v, off + 1, &p))) return r;
    if (c & 1) *p = (uint8_t)(val >> 4); else *p = (uint8_t)((*p & 0xF0) | ((val >> 8) & 0x0F));
    v->fsec_dirty = 1;
    return 0;
}

static uint32_t eoc(fat_vol_t* v) { return v->type == 32 ? 0x0FFFFFFF : v->type == 16 ? 0xFFFF : 0xFFF; }
static int is_eoc(fat_vol_t* v, uint32_t x) { return x >= (v->type == 32 ? 0x0FFFFFF8u : v->type == 16 ? 0xFFF8u : 0xFF8u); }
static int is_bad(fat_vol_t* v, uint32_t x) { return x == (v->type == 32 ? 0x0FFFFFF7u : v->type == 16 ? 0xFFF7u : 0xFF7u); }

/* Следующий кластер цепочки: 0 - конец; ошибка - отрицательное. */
static int chain_next(fat_vol_t* v, uint32_t c, uint32_t* next) {
    uint32_t x; int r = fat_get(v, c, &x); if (r) return r;
    if (is_eoc(v, x)) { *next = 0; return 0; }
    if (x < 2 || x >= v->clusters + 2 || is_bad(v, x)) return KFS_ERR_CORRUPT;
    *next = x;
    return 0;
}

static uint64_t clus_sec(fat_vol_t* v, uint32_t c) { return v->data_start + (uint64_t)(c - 2) * v->spc; }

static int count_free(fat_vol_t* v) {
    uint32_t n = 0;
    for (uint32_t c = 2; c < v->clusters + 2; c++) { uint32_t x; int r = fat_get(v, c, &x); if (r) return r; if (!x) n++; }
    v->free_count = n; v->free_known = 1;
    return 0;
}

/* Цепочка из n кластеров (не обязательно подряд). Ошибка - всё выделенное возвращается. */
static int chain_free(fat_vol_t* v, uint32_t c);
static int chain_alloc(fat_vol_t* v, uint32_t n, uint32_t* first) {
    *first = 0;
    if (!n) return 0;
    if (v->free_known && v->free_count < n) return KFS_ERR_NOSPC;
    uint32_t prev = 0, got = 0, c = v->next_free >= 2 && v->next_free < v->clusters + 2 ? v->next_free : 2;
    for (uint32_t scanned = 0; scanned < v->clusters && got < n; scanned++) {
        uint32_t x; int r = fat_get(v, c, &x);
        if (r) { if (*first) chain_free(v, *first); *first = 0; return r; }
        if (x == 0) {
            if ((r = fat_set(v, c, eoc(v)))) return r;
            if (prev) { if ((r = fat_set(v, prev, c))) return r; } else *first = c;
            prev = c; got++;
        }
        if (++c >= v->clusters + 2) c = 2;
    }
    if (got < n) { if (*first) chain_free(v, *first); *first = 0; return KFS_ERR_NOSPC; }
    v->next_free = c;
    if (v->free_known) v->free_count -= n;
    return 0;
}

static int chain_free(fat_vol_t* v, uint32_t c) {
    for (uint32_t guard = 0; c && guard <= v->clusters; guard++) {
        uint32_t nx; int r = chain_next(v, c, &nx);
        if (r == KFS_ERR_CORRUPT) nx = 0; else if (r) return r;
        if ((r = fat_set(v, c, 0))) return r;
        if (v->free_known) v->free_count++;
        c = nx;
    }
    return 0;
}

static int fsinfo_store(fat_vol_t* v) {
    if (v->type != 32 || !v->fsinfo) return 0;
    int r = sec_load(v, v->fsinfo); if (r) return r;
    if (rd32(v->sec) != 0x41615252u || rd32(v->sec + 484) != 0x61417272u) return 0;
    wr32(v->sec + 488, v->free_known ? v->free_count : 0xFFFFFFFFu);
    wr32(v->sec + 492, v->next_free);
    return sec_store(v);
}

int fat_flush(fat_vol_t* v) {
    int r = fsec_flush(v); if (r) return r;
    r = fsinfo_store(v); if (r) return r;
    if (v->dev->flush) v->dev->flush(v->dev);
    return 0;
}

/* ---------------- монтирование ---------------- */

int fat_mount(fat_vol_t* v, blk_dev_t* dev, uint64_t base, uint64_t sectors) {
    mset(v, 0, sizeof *v);
    v->dev = dev; v->base = base;
    int r = sec_load(v, 0); if (r) return r;
    const uint8_t* b = v->sec;
    if (rd16(b + 510) != 0xAA55 || (b[0] != 0xEB && b[0] != 0xE9)) return KFS_ERR_NOFS;
    uint32_t bps = rd16(b + 11), spc = b[13], rsvd = rd16(b + 14), nfats = b[16], rents = rd16(b + 17);
    uint32_t tot = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    uint32_t fatsz = rd16(b + 22) ? rd16(b + 22) : rd32(b + 36);
    if (bps != FAT_SECTOR) return KFS_ERR_NOFS;                     /* устройства с другим сектором - позже */
    if (!spc || (spc & (spc - 1)) || spc > 128 || !rsvd || !nfats || nfats > 4 || !fatsz || !tot) return KFS_ERR_NOFS;
    if (sectors && tot > sectors) return KFS_ERR_NOFS;
    uint32_t root_secs = (rents * 32 + bps - 1) / bps;
    uint64_t meta = (uint64_t)rsvd + (uint64_t)nfats * fatsz + root_secs;
    if (meta >= tot) return KFS_ERR_NOFS;
    uint32_t clusters = (uint32_t)((tot - meta) / spc);
    v->type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    if (v->type == 32 && (rents || !rd32(b + 36))) return KFS_ERR_NOFS;
    if (v->type != 32 && !rents) return KFS_ERR_NOFS;
    uint64_t fat_bytes_needed = v->type == 32 ? (uint64_t)(clusters + 2) * 4 : v->type == 16 ? (uint64_t)(clusters + 2) * 2 : (uint64_t)(clusters + 2) * 3 / 2;
    if ((uint64_t)fatsz * bps < fat_bytes_needed) return KFS_ERR_NOFS;
    v->spc = spc; v->rsvd = rsvd; v->nfats = nfats; v->root_ents = rents; v->fatsz = fatsz; v->total = tot;
    v->root_secs = root_secs; v->data_start = (uint32_t)meta; v->clusters = clusters;
    v->root_clus = v->type == 32 ? rd32(b + 44) : 0;
    if (v->type == 32 && (v->root_clus < 2 || v->root_clus >= clusters + 2)) return KFS_ERR_NOFS;
    const uint8_t* lab = v->type == 32 ? b + 71 : b + 43;
    for (int i = 0; i < 11; i++) v->label[i] = (char)(lab[i] >= 0x20 && lab[i] < 0x7F ? lab[i] : ' ');
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
    v->label[11] = 0;
    v->next_free = 2;
    if (v->type == 32) {
        v->fsinfo = rd16(b + 48);
        if (v->fsinfo && v->fsinfo < rsvd && sec_load(v, v->fsinfo) == 0 && rd32(v->sec) == 0x41615252u && rd32(v->sec + 484) == 0x61417272u) {
            uint32_t fc = rd32(v->sec + 488), nf = rd32(v->sec + 492);
            if (fc <= clusters) { v->free_count = fc; v->free_known = 1; }
            if (nf >= 2 && nf < clusters + 2) v->next_free = nf;
        } else v->fsinfo = 0;
    }
    v->mounted = 1;
    return 0;
}

void fat_info(fat_vol_t* v, uint64_t* total_kb, uint64_t* free_kb) {
    *total_kb = (uint64_t)v->clusters * v->spc / 2;
    if (!v->free_known) count_free(v);
    *free_kb = (uint64_t)v->free_count * v->spc / 2;
}

/* ---------------- имена ---------------- */

static uint32_t cp866(uint8_t c) {                                    /* короткие имена: русская кодовая страница DOS */
    if (c < 0x80) return c;
    if (c <= 0xAF) return 0x410 + (c - 0x80);
    if (c >= 0xE0 && c <= 0xEF) return 0x440 + (c - 0xE0);
    if (c == 0xF0) return 0x401;
    if (c == 0xF1) return 0x451;
    return '_';
}

static int put_utf8(char* o, int p, int cap, uint32_t cp) {
    if (cp < 0x80) { if (p + 1 >= cap) return p; o[p++] = (char)cp; }
    else if (cp < 0x800) { if (p + 2 >= cap) return p; o[p++] = (char)(0xC0 | cp >> 6); o[p++] = (char)(0x80 | (cp & 63)); }
    else { if (p + 3 >= cap) return p; o[p++] = (char)(0xE0 | cp >> 12); o[p++] = (char)(0x80 | ((cp >> 6) & 63)); o[p++] = (char)(0x80 | (cp & 63)); }
    o[p] = 0;
    return p;
}

static uint32_t next_cp(const char** s) {
    const uint8_t* p = (const uint8_t*)*s; uint32_t c = *p++;
    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) { c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char*)p;
    return c;
}

static uint32_t fold(uint32_t c) {                                     /* без учёта регистра: латиница и кириллица */
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c == 0x451) return 0x401;
    return c;
}

static int name_eq(const char* a, const char* b) {
    while (*a && *b) { if (fold(next_cp(&a)) != fold(next_cp(&b))) return 0; }
    return *a == *b;
}

uint8_t fat_lfn_checksum(const uint8_t s[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static int short_ok(uint32_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '-' ||
           c == '@' || c == '^' || c == '_' || c == '`' || c == '{' || c == '}' || c == '~';
}

int fat_short_name(const char* name, uint8_t out[11], uint8_t* nt) {
    mset(out, ' ', 11); *nt = 0;
    int bl = 0, el = 0, dot = 0, up_b = 0, lo_b = 0, up_e = 0, lo_e = 0;
    if (name[0] == '.') return 0;
    for (const char* p = name; *p; ) {
        uint32_t c = next_cp(&p);
        if (c == '.') { if (dot) return 0; dot = 1; continue; }
        if (!short_ok(c)) return 0;
        int lower = c >= 'a' && c <= 'z', upper = c >= 'A' && c <= 'Z';
        uint8_t u = (uint8_t)(lower ? c - 32 : c);
        if (!dot) { if (bl >= 8) return 0; out[bl++] = u; up_b |= upper; lo_b |= lower; }
        else { if (el >= 3) return 0; out[8 + el++] = u; up_e |= upper; lo_e |= lower; }
    }
    if (!bl || (dot && !el)) return 0;
    if ((up_b && lo_b) || (up_e && lo_e)) return 0;                  /* смешанный регистр - нужен LFN */
    *nt = (uint8_t)((lo_b ? 0x08 : 0) | (lo_e ? 0x10 : 0));
    return 1;
}

static void short_display(const uint8_t* e, char* out, int cap) {
    int p = 0; out[0] = 0;
    uint8_t nt = e[12];
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        uint8_t c = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
        uint32_t cp = cp866(c);
        if ((nt & 0x08) && cp >= 'A' && cp <= 'Z') cp += 32;
        p = put_utf8(out, p, cap, cp);
    }
    if (e[8] != ' ') {
        p = put_utf8(out, p, cap, '.');
        for (int i = 8; i < 11 && e[i] != ' '; i++) { uint32_t cp = cp866(e[i]); if ((nt & 0x10) && cp >= 'A' && cp <= 'Z') cp += 32; p = put_utf8(out, p, cap, cp); }
    }
}

/* ---------------- каталоги ---------------- */

typedef struct {
    char name[256];
    uint8_t attr, s83[11];
    uint32_t clus, size;
    uint32_t first, last;          /* номера записей: первая (LFN или короткая) и короткая */
} dent_t;

/* Сектор записи idx каталога (dir = 0 - корень FAT12/16). 0 - есть; KFS_ERR_NOENT - за концом. */
static int dir_slot(fat_vol_t* v, uint32_t dir, uint32_t idx, uint64_t* lba, unsigned* off) {
    uint32_t per_sec = FAT_SECTOR / 32;
    if (idx >= 65536) return KFS_ERR_CORRUPT;                          /* больше записей в каталоге FAT не бывает (зацикленная цепочка) */
    if (!dir) {
        if (idx >= v->root_ents) return KFS_ERR_NOENT;
        *lba = v->rsvd + (uint64_t)v->nfats * v->fatsz + idx / per_sec; *off = (idx % per_sec) * 32;
        return 0;
    }
    uint32_t per_clus = per_sec * v->spc, n = idx / per_clus, c = dir;
    for (uint32_t i = 0; i < n; i++) { uint32_t nx; int r = chain_next(v, c, &nx); if (r) return r; if (!nx) return KFS_ERR_NOENT; c = nx; }
    uint32_t in = idx % per_clus;
    *lba = clus_sec(v, c) + in / per_sec; *off = (in % per_sec) * 32;
    return 0;
}

/* Следующая запись (имя - длинное, если есть и сходится контрольная сумма). 1 - есть, 0 - конец, <0 - ошибка. */
static int dir_next(fat_vol_t* v, uint32_t dir, uint32_t* idx, dent_t* d, int want_dots) {
    uint16_t lfn[260]; int lfn_n = 0, lfn_ok = 0; uint8_t lfn_sum = 0; uint32_t lfn_first = 0;
    for (;; (*idx)++) {
        uint64_t lba; unsigned off;
        int r = dir_slot(v, dir, *idx, &lba, &off);
        if (r == KFS_ERR_NOENT) return 0;
        if (r) return r;
        if ((r = sec_load(v, lba))) return r;
        const uint8_t* e = v->sec + off;
        if (e[0] == 0x00) return 0;                                   /* дальше записей нет */
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == ATTR_LFN) {
            uint8_t seq = e[0] & 0x1F;
            if (e[0] & 0x40) { lfn_n = seq; lfn_ok = seq >= 1 && seq <= 20; lfn_sum = e[13]; lfn_first = *idx; for (int i = 0; i < 260; i++) lfn[i] = 0xFFFF; }
            else if (!lfn_ok || e[13] != lfn_sum) { lfn_ok = 0; continue; }
            if (!lfn_ok || seq < 1 || seq > lfn_n) { lfn_ok = 0; continue; }
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) lfn[(seq - 1) * 13 + k] = rd16(e + pos[k]);
            continue;
        }
        if (e[11] & ATTR_VOL) { lfn_ok = 0; continue; }
        if (e[0] == '.' && !want_dots) { lfn_ok = 0; continue; }
        mset(d, 0, sizeof *d);
        mcpy(d->s83, e, 11);
        d->attr = e[11];
        d->clus = (uint32_t)rd16(e + 26) | (v->type == 32 ? (uint32_t)rd16(e + 20) << 16 : 0);
        d->size = rd32(e + 28);
        d->last = *idx;
        d->first = *idx;
        if (lfn_ok && fat_lfn_checksum(e) == lfn_sum) {
            int p = 0;
            for (int k = 0; k < lfn_n * 13 && lfn[k] && lfn[k] != 0xFFFF; k++) p = put_utf8(d->name, p, sizeof d->name, lfn[k]);
            d->first = lfn_first;
        }
        if (!d->name[0]) short_display(e, d->name, sizeof d->name);
        lfn_ok = 0;
        (*idx)++;
        return 1;
    }
}

static int dir_find(fat_vol_t* v, uint32_t dir, const char* name, dent_t* d) {
    uint32_t idx = 0;
    for (;;) {
        int r = dir_next(v, dir, &idx, d, 0);
        if (r < 0) return r;
        if (r == 0) return KFS_ERR_NOENT;
        if (name_eq(d->name, name)) return 0;
        char s[16]; short_display(d->s83, s, sizeof s);
        if (name_eq(s, name)) return 0;                              /* по короткому имени тоже */
    }
}

/* Путь -> запись. Корень - особый: dir_out = кластер корня, is_root = 1. */
static int resolve(fat_vol_t* v, const char* path, dent_t* d, uint32_t* parent, int* is_root) {
    uint32_t dir = v->root_clus;
    *is_root = 1; *parent = dir;
    while (*path == '/') path++;
    if (!*path) { mset(d, 0, sizeof *d); d->attr = ATTR_DIR; d->clus = dir; return 0; }
    *is_root = 0;
    for (;;) {
        char comp[256]; int n = 0;
        while (*path && *path != '/') { if (n < 255) comp[n++] = *path; path++; }
        comp[n] = 0;
        while (*path == '/') path++;
        *parent = dir;
        int r = dir_find(v, dir, comp, d); if (r) return r;
        if (!*path) return 0;
        if (!(d->attr & ATTR_DIR)) return KFS_ERR_NOTDIR;
        dir = d->clus;
        if (v->type == 32 && dir == 0) dir = v->root_clus;            /* «..» в корень записывается нулём */
    }
}

static int split(const char* path, char* parent, int pcap, const char** leaf) {
    int n = 0, last = -1;
    for (int i = 0; path[i]; i++) if (path[i] == '/' && path[i + 1]) last = i;
    *leaf = path + last + 1;
    while (**leaf == '/') (*leaf)++;
    for (int i = 0; i <= last && n < pcap - 1; i++) parent[n++] = path[i];
    if (!n) parent[n++] = '/';
    parent[n] = 0;
    int l = 0; while ((*leaf)[l] && (*leaf)[l] != '/') l++;
    return l;
}

/* ---------------- чтение ---------------- */

int fat_stat(fat_vol_t* v, const char* path, kfs_stat_t* st) {
    if (!v->mounted) return KFS_ERR_NOFS;
    dent_t d; uint32_t par; int root;
    int r = resolve(v, path, &d, &par, &root); if (r) return r;
    st->type = (d.attr & ATTR_DIR) ? KFS_DIR : KFS_FILE;
    st->size = (d.attr & ATTR_DIR) ? 0 : d.size;
    st->ino = d.clus;
    return 0;
}

int fat_list(fat_vol_t* v, const char* path, kfs_list_cb cb, void* ctx) {
    if (!v->mounted) return KFS_ERR_NOFS;
    dent_t d; uint32_t par; int root;
    int r = resolve(v, path, &d, &par, &root); if (r) return r;
    if (!(d.attr & ATTR_DIR)) return KFS_ERR_NOTDIR;
    uint32_t dir = root ? v->root_clus : d.clus, idx = 0;
    if (v->type == 32 && !dir) dir = v->root_clus;
    for (;;) {
        r = dir_next(v, dir, &idx, &d, 0);
        if (r < 0) return r;
        if (!r) return 0;
        if (d.attr & ATTR_HIDDEN && d.attr & ATTR_SYS) continue;        /* служебные файлы системы */
        kfs_stat_t st = { (d.attr & ATTR_DIR) ? (uint32_t)KFS_DIR : (uint32_t)KFS_FILE, (d.attr & ATTR_DIR) ? 0 : d.size, d.clus };
        if (cb(ctx, d.name, &st)) return 0;
    }
}

int64_t fat_read(fat_vol_t* v, const char* path, uint64_t off, void* buf, size_t len) {
    if (!v->mounted) return KFS_ERR_NOFS;
    dent_t d; uint32_t par; int root;
    int r = resolve(v, path, &d, &par, &root); if (r) return r;
    if (d.attr & ATTR_DIR) return KFS_ERR_ISDIR;
    if (off >= d.size) return 0;
    if (len > d.size - off) len = (size_t)(d.size - off);
    uint32_t csize = v->spc * FAT_SECTOR, c = d.clus;
    for (uint64_t skip = off / csize; skip && c; skip--) { uint32_t nx; if ((r = chain_next(v, c, &nx))) return r; c = nx; }
    size_t done = 0;
    uint64_t pos = off;
    while (done < len) {
        if (c < 2) return KFS_ERR_CORRUPT;
        uint32_t in = (uint32_t)(pos % csize);
        uint64_t s = clus_sec(v, c) + in / FAT_SECTOR;
        uint32_t so = in % FAT_SECTOR;
        if (so == 0 && len - done >= FAT_SECTOR) {                     /* целые секторы - прямо в буфер */
            uint32_t want = (uint32_t)((len - done) / FAT_SECTOR > FAT_RUN_MAX ? FAT_RUN_MAX : (len - done) / FAT_SECTOR);
            uint32_t n = (csize - in) / FAT_SECTOR, last = c;
            if (n > want) n = want;
            while (n < want) {                                         /* кластеры, идущие подряд, - одним запросом */
                uint32_t nx; if ((r = chain_next(v, last, &nx))) return r;
                if (nx != last + 1 || nx < 2) break;
                last = nx; n += want - n < v->spc ? want - n : v->spc;
            }
            if ((r = dev_read(v, s, n, (uint8_t*)buf + done))) return r;
            done += (size_t)n * FAT_SECTOR; pos += (uint64_t)n * FAT_SECTOR;
            c = last;
        } else {
            if ((r = sec_load(v, s))) return r;
            uint32_t n = FAT_SECTOR - so; if (n > len - done) n = (uint32_t)(len - done);
            mcpy((uint8_t*)buf + done, v->sec + so, n);
            done += n; pos += n;
        }
        if (pos % csize == 0 && done < len) { uint32_t nx; if ((r = chain_next(v, c, &nx))) return r; c = nx; }
    }
    return (int64_t)done;
}

/* ---------------- запись ---------------- */

static void stamp(uint8_t* e, int create) {
    uint16_t dt, tm; clock_fn(&dt, &tm);
    if (create) { wr16(e + 14, tm); wr16(e + 16, dt); e[13] = 0; }
    wr16(e + 18, dt); wr16(e + 22, tm); wr16(e + 24, dt);
}

static int write_data(fat_vol_t* v, uint32_t c, const uint8_t* data, size_t len) {
    uint32_t csize = v->spc * FAT_SECTOR;
    size_t done = 0;
    while (done < len) {
        if (c < 2) return KFS_ERR_CORRUPT;
        size_t n = len - done < csize ? len - done : csize;
        uint32_t last = c;
        int r;
        while (done + n < len && n % csize == 0 && n < (size_t)FAT_RUN_MAX * FAT_SECTOR) {   /* подряд идущие кластеры - одной записью */
            uint32_t nx; if ((r = chain_next(v, last, &nx))) return r;
            if (nx != last + 1 || nx < 2) break;
            last = nx; n += len - done - n < csize ? len - done - n : csize;
        }
        uint32_t full = (uint32_t)(n / FAT_SECTOR);
        if (full && (r = dev_write(v, clus_sec(v, c), full, data + done))) return r;
        if (n % FAT_SECTOR) {                                           /* хвост: остаток сектора - нули */
            uint8_t tail[FAT_SECTOR]; mset(tail, 0, sizeof tail); mcpy(tail, data + done + (size_t)full * FAT_SECTOR, n % FAT_SECTOR);
            if ((r = dev_write(v, clus_sec(v, c) + full, 1, tail))) return r;
        }
        v->sec_valid = 0;
        done += n;
        if (done < len) { uint32_t nx; if ((r = chain_next(v, last, &nx))) return r; c = nx; }
    }
    return 0;
}

static int zero_cluster(fat_vol_t* v, uint32_t c) {
    uint8_t z[FAT_SECTOR]; mset(z, 0, sizeof z);
    for (uint32_t i = 0; i < v->spc; i++) { int r = dev_write(v, clus_sec(v, c) + i, 1, z); if (r) return r; }
    v->sec_valid = 0;
    return 0;
}

/* Найти n свободных записей подряд (при нехватке каталог-цепочка удлиняется на кластер). */
static int dir_room(fat_vol_t* v, uint32_t dir, uint32_t n, uint32_t* at) {
    uint32_t run = 0, start = 0;
    for (uint32_t idx = 0;; idx++) {
        uint64_t lba; unsigned off;
        int r = dir_slot(v, dir, idx, &lba, &off);
        if (r == KFS_ERR_NOENT) {
            if (!dir) return KFS_ERR_NOSPC;                            /* корень FAT12/16 не растёт */
            uint32_t last = dir, nx;
            for (uint32_t g = 0; g <= v->clusters; g++) { if ((r = chain_next(v, last, &nx))) return r; if (!nx) break; last = nx; }
            uint32_t nc; if ((r = chain_alloc(v, 1, &nc))) return r;
            if ((r = zero_cluster(v, nc))) return r;
            if ((r = fat_set(v, last, nc))) return r;
            if (!run) start = idx;
            *at = start;                                              /* новый кластер - весь свободен */
            return 0;
        }
        if (r) return r;
        if ((r = sec_load(v, lba))) return r;
        uint8_t b0 = v->sec[off];
        if (b0 == 0x00 || b0 == 0xE5) { if (!run) start = idx; if (++run == n) { *at = start; return 0; } }
        else run = 0;
    }
}

static int dir_put(fat_vol_t* v, uint32_t dir, uint32_t idx, const uint8_t e[32]) {
    uint64_t lba; unsigned off;
    int r = dir_slot(v, dir, idx, &lba, &off); if (r) return r;
    if ((r = sec_load(v, lba))) return r;
    mcpy(v->sec + off, e, 32);
    return sec_store(v);
}

static int short_exists(fat_vol_t* v, uint32_t dir, const uint8_t s[11]) {
    uint32_t idx = 0; dent_t d;
    for (;;) {
        int r = dir_next(v, dir, &idx, &d, 1);
        if (r <= 0) return r < 0 ? r : 0;
        int same = 1; for (int i = 0; i < 11; i++) if (d.s83[i] != s[i]) same = 0;
        if (same) return 1;
    }
}

static int valid_long(const char* n) {
    int cnt = 0;
    if (!n[0] || (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))) return 0;
    for (const char* p = n; *p; ) {
        uint32_t c = next_cp(&p);
        if (c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|' || c > 0xFFFF) return 0;
        cnt++;
    }
    return cnt <= 255;
}

/* Новая запись (длинное имя + короткая). first_clus/size/attr - содержимое короткой записи. */
static int dir_create(fat_vol_t* v, uint32_t dir, const char* name, uint8_t attr, uint32_t first_clus, uint32_t size, uint32_t* short_idx) {
    if (!valid_long(name)) return KFS_ERR_INVAL;
    uint8_t s[11], nt = 0;
    uint16_t u[256]; int un = 0;
    for (const char* p = name; *p && un < 255; ) u[un++] = (uint16_t)next_cp(&p);
    int need_lfn = !fat_short_name(name, s, &nt);
    if (need_lfn) {                                                   /* основа + ~N */
        uint8_t base[8], ext[3]; int bl = 0, el = 0;
        const char* lastdot = 0; for (const char* p = name; *p; p++) if (*p == '.') lastdot = p;
        if (lastdot == name) lastdot = 0;
        for (const char* p = name; *p && (!lastdot || p < lastdot) && bl < 6; ) {
            uint32_t c = next_cp(&p);
            if (c == ' ' || c == '.') continue;
            base[bl++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : short_ok(c) ? c : '_');
        }
        if (lastdot) for (const char* p = lastdot + 1; *p && el < 3; ) {
            uint32_t c = next_cp(&p);
            if (c == ' ') continue;
            ext[el++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : short_ok(c) ? c : '_');
        }
        if (!bl) base[bl++] = '_';
        int found = 0;
        for (uint32_t k = 1; k < 1000000 && !found; k++) {
            char num[8]; int nl = 0; uint32_t t = k; char tmp[8]; int tl = 0;
            do { tmp[tl++] = (char)('0' + t % 10); t /= 10; } while (t);
            num[nl++] = '~'; while (tl) num[nl++] = tmp[--tl];
            int keep = 8 - nl; if (keep > bl) keep = bl;
            mset(s, ' ', 11);
            for (int i = 0; i < keep; i++) s[i] = base[i];
            for (int i = 0; i < nl; i++) s[keep + i] = (uint8_t)num[i];
            for (int i = 0; i < el; i++) s[8 + i] = ext[i];
            int ex = short_exists(v, dir, s);
            if (ex < 0) return ex;
            if (!ex) found = 1;
        }
        if (!found) return KFS_ERR_EXIST;
        nt = 0;
    } else {
        int ex = short_exists(v, dir, s);
        if (ex < 0) return ex;
        if (ex) return KFS_ERR_EXIST;
    }
    uint32_t nl = need_lfn ? (uint32_t)(un + 12) / 13 : 0, at;
    int r = dir_room(v, dir, nl + 1, &at); if (r) return r;
    uint8_t chk = fat_lfn_checksum(s);
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (uint32_t k = 0; k < nl; k++) {                               /* LFN: последний кусок первым */
        uint32_t seq = nl - k;
        uint8_t e[32]; mset(e, 0, 32);
        e[0] = (uint8_t)(seq | (k == 0 ? 0x40 : 0)); e[11] = ATTR_LFN; e[13] = chk;
        for (int j = 0; j < 13; j++) {
            int ci = (int)(seq - 1) * 13 + j;
            uint16_t ch = ci < un ? u[ci] : ci == un ? 0 : 0xFFFF;
            wr16(e + pos[j], ch);
        }
        if ((r = dir_put(v, dir, at + k, e))) return r;
    }
    uint8_t e[32]; mset(e, 0, 32);
    mcpy(e, s, 11); e[11] = attr; e[12] = nt;
    wr16(e + 26, (uint16_t)first_clus); wr16(e + 20, (uint16_t)(first_clus >> 16)); wr32(e + 28, size);
    stamp(e, 1);
    if ((r = dir_put(v, dir, at + nl, e))) return r;
    if (short_idx) *short_idx = at + nl;
    return 0;
}

static int parent_dir(fat_vol_t* v, const char* path, uint32_t* dir, const char** leaf, char* leafbuf) {
    char par[256]; const char* lf;
    int ll = split(path, par, sizeof par, &lf);
    if (!ll || ll > 255) return KFS_ERR_INVAL;
    for (int i = 0; i < ll; i++) leafbuf[i] = lf[i];
    leafbuf[ll] = 0; *leaf = leafbuf;
    dent_t d; uint32_t pp; int root;
    int r = resolve(v, par, &d, &pp, &root); if (r) return r;
    if (!(d.attr & ATTR_DIR)) return KFS_ERR_NOTDIR;
    *dir = root ? v->root_clus : (d.clus ? d.clus : v->root_clus);
    return 0;
}

int fat_write(fat_vol_t* v, const char* path, const void* data, size_t len) {
    if (!v->mounted) return KFS_ERR_NOFS;
    if (len > 0xFFFFFFFFu) return KFS_ERR_FBIG;
    uint32_t dir; const char* leaf; char lb[256];
    int r = parent_dir(v, path, &dir, &leaf, lb); if (r) return r;
    dent_t old; int have = dir_find(v, dir, leaf, &old);
    if (have < 0 && have != KFS_ERR_NOENT) return have;
    have = have == 0;
    if (have && (old.attr & ATTR_DIR)) return KFS_ERR_ISDIR;
    if (have && (old.attr & ATTR_RO)) return KFS_ERR_INVAL;
    uint32_t csize = v->spc * FAT_SECTOR, need = (uint32_t)((len + csize - 1) / csize), first = 0;
    if ((r = chain_alloc(v, need, &first))) { fat_flush(v); return r; }
    if ((r = write_data(v, first, (const uint8_t*)data, len))) { chain_free(v, first); fat_flush(v); return r; }
    if ((r = fsec_flush(v))) return r;                                 /* таблица - до записи каталога */
    if (have) {
        uint64_t lba; unsigned off;
        if ((r = dir_slot(v, dir, old.last, &lba, &off)) || (r = sec_load(v, lba))) return r;
        uint8_t* e = v->sec + off;
        wr16(e + 26, (uint16_t)first); wr16(e + 20, (uint16_t)(first >> 16)); wr32(e + 28, (uint32_t)len);
        e[11] |= ATTR_ARCH; stamp(e, 0);
        if ((r = sec_store(v))) return r;
        if (old.clus) chain_free(v, old.clus);
    } else if ((r = dir_create(v, dir, leaf, ATTR_ARCH, first, (uint32_t)len, 0))) {
        chain_free(v, first); fat_flush(v); return r;
    }
    return fat_flush(v);
}

int fat_mkdir(fat_vol_t* v, const char* path) {
    if (!v->mounted) return KFS_ERR_NOFS;
    uint32_t dir; const char* leaf; char lb[256];
    int r = parent_dir(v, path, &dir, &leaf, lb); if (r) return r;
    dent_t d; r = dir_find(v, dir, leaf, &d);
    if (r == 0) return KFS_ERR_EXIST;
    if (r != KFS_ERR_NOENT) return r;
    uint32_t c; if ((r = chain_alloc(v, 1, &c))) return r;
    if ((r = zero_cluster(v, c))) { chain_free(v, c); fat_flush(v); return r; }
    uint8_t e[32];
    mset(e, 0, 32); mset(e, ' ', 11); e[0] = '.'; e[11] = ATTR_DIR; wr16(e + 26, (uint16_t)c); wr16(e + 20, (uint16_t)(c >> 16)); stamp(e, 1);
    if ((r = dev_read(v, clus_sec(v, c), 1, v->sec))) return r;
    v->sec_lba = clus_sec(v, c); v->sec_valid = 1;
    mcpy(v->sec, e, 32);
    uint32_t pc = dir == v->root_clus ? 0 : dir;                      /* «..» на корень - ноль */
    e[1] = '.'; wr16(e + 26, (uint16_t)pc); wr16(e + 20, (uint16_t)(pc >> 16));
    mcpy(v->sec + 32, e, 32);
    if ((r = sec_store(v))) return r;
    if ((r = fsec_flush(v))) return r;
    if ((r = dir_create(v, dir, leaf, ATTR_DIR, c, 0, 0))) { chain_free(v, c); fat_flush(v); return r; }
    return fat_flush(v);
}

int fat_unlink(fat_vol_t* v, const char* path) {
    if (!v->mounted) return KFS_ERR_NOFS;
    uint32_t dir; const char* leaf; char lb[256];
    int r = parent_dir(v, path, &dir, &leaf, lb); if (r) return r;
    dent_t d; if ((r = dir_find(v, dir, leaf, &d))) return r;
    if (d.attr & ATTR_DIR) {
        uint32_t idx = 0; dent_t x;
        int k = dir_next(v, d.clus, &idx, &x, 0);
        if (k < 0) return k;
        if (k > 0) return KFS_ERR_NOTEMPTY;
    }
    for (uint32_t i = d.first; i <= d.last; i++) {                     /* LFN и короткая запись - удалены */
        uint64_t lba; unsigned off;
        if ((r = dir_slot(v, dir, i, &lba, &off)) || (r = sec_load(v, lba))) return r;
        v->sec[off] = 0xE5;
        if ((r = sec_store(v))) return r;
    }
    if (d.clus >= 2) chain_free(v, d.clus);
    return fat_flush(v);
}
