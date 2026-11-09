/* Разбор и проверка пакета драйвера (kdrv_format.h). Чистые функции без обращений к ядру: их используют ядро (kdrv.c),
   хост-инструмент tools/kdrvpack и хост-тест. Входу не доверяют: каждое поле проверяется, имя - символы и длина. */
#include "kdrv_format.h"

static void cpy(void* d, const void* s, uint32_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }

static int name_ok(const char* n, unsigned cap) {
    unsigned i = 0;
    while (i < cap && n[i]) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        i++;
    }
    return i >= 1 && i <= KDRV_NAME_MAX && i < cap;     /* нуль внутри поля обязателен */
}

int kdrv_header_check(const void* file, uint32_t len, kdrv_header_t* out) {
    kdrv_header_t h;
    if (len < sizeof h) return KDRV_E_SHORT;
    cpy(&h, file, sizeof h);                            /* копия: проверяем то, что потом используем */
    if (h.magic != KDRV_MAGIC) return KDRV_E_MAGIC;
    if (h.format != KDRV_FORMAT || h.hdr_size != sizeof h) return KDRV_E_FORMAT;
    if (!name_ok(h.name, sizeof h.name)) return KDRV_E_NAME;
    if (h.caps & ~(uint32_t)CAP_DRIVER_) return KDRV_E_CAPS;      /* драйвер не может просить права обычных программ */
    if (h.nmatch < 1 || h.nmatch > KDRV_MATCH_MAX) return KDRV_E_MATCH;
    for (uint32_t i = 0; i < h.nmatch; i++) {
        const kdrv_match_t* m = &h.match[i];
        if (m->vendor == KDRV_ANY && m->device == KDRV_ANY && m->cls_mask == 0) return KDRV_E_MATCH;   /* подошло бы всё */
        if (m->vendor == KDRV_ANY && m->device != KDRV_ANY) return KDRV_E_MATCH;                          /* номер устройства без изготовителя */
        if (m->cls_mask & ~0x00FFFFFFu) return KDRV_E_MATCH;
    }
    if (h.elf_size == 0 || h.elf_size > KDRV_ELF_MAX || len - sizeof h != h.elf_size) return KDRV_E_SIZE;
    if (out) *out = h;
    return KDRV_OK;
}

int kdrv_rec_check(const void* rec, uint32_t len, kdrv_rec_t* out) {
    kdrv_rec_t r;
    if (len < sizeof r) return KDRV_E_SHORT;
    cpy(&r, rec, sizeof r);
    if (r.magic != KREC_MAGIC) return KDRV_E_MAGIC;
    if (r.format != KDRV_FORMAT || r.rec_size != sizeof r || len != sizeof r) return KDRV_E_FORMAT;
    unsigned z = 0;
    while (z < sizeof r.source && r.source[z]) z++;
    if (z == sizeof r.source) return KDRV_E_FORMAT;     /* строка без нуля */
    if (r.confirmed != KREC_CONFIRMED && r.confirmed != KREC_CONFIRMED_PUBLISHER) return KDRV_E_UNCONFIRMED;
    if (r.caps_ok & ~(uint32_t)CAP_DRIVER_) return KDRV_E_CAPS;
    if (out) *out = r;
    return KDRV_OK;
}

int kdrv_rec_publisher_required(const kdrv_rec_t* rec) {
    return rec && rec->magic == KREC_MAGIC && rec->confirmed == KREC_CONFIRMED_PUBLISHER;
}

int kdrv_match_dev(const kdrv_header_t* h, uint16_t vendor, uint16_t device, uint32_t cls) {
    for (uint32_t i = 0; i < h->nmatch && i < KDRV_MATCH_MAX; i++) {
        const kdrv_match_t* m = &h->match[i];
        if (m->vendor != KDRV_ANY && m->vendor != vendor) continue;
        if (m->device != KDRV_ANY && m->device != device) continue;
        if ((cls & m->cls_mask) != (m->cls & m->cls_mask)) continue;
        return 1;
    }
    return 0;
}

int kdrv_dma_policy_allow(uint32_t caps, int isolated) {
    if (caps & (CAP_DMA | CAP_IRQ)) return isolated != 0;
    return 1;
}

const char* kdrv_strerror(int e) {
    switch (e) {
    case KDRV_OK: return "ok";
    case KDRV_E_SHORT: return "файл короче заголовка";
    case KDRV_E_MAGIC: return "нет сигнатуры формата";
    case KDRV_E_FORMAT: return "неизвестная версия формата";
    case KDRV_E_NAME: return "недопустимое имя драйвера";
    case KDRV_E_CAPS: return "запрошены права, которых нет у драйверов";
    case KDRV_E_MATCH: return "неверный список устройств";
    case KDRV_E_SIZE: return "размер образа не совпал с заголовком";
    case KDRV_E_UNCONFIRMED: return "установка не подтверждена пользователем";
    case KDRV_E_HASH: return "SHA-256 не совпал с записью .rec";
    case KDRV_E_DMA_UNSAFE: return "DMA/MSI запрещены: для устройства нет подтверждённой изоляции IOMMU";
    case KDRV_E_SIGNATURE: return "подпись издателя недействительна";
    case KDRV_E_ROLLBACK: return "версия ниже сохранённого пола: откат запрещён";
    case KDRV_E_STATE: return "файл состояния /var/kdrv_state повреждён или нечитаем: драйверы не запускаются (drv state-reset после проверки диска)";
    default: return "?";
    }
}

/* Порты I/O BAR: чистая проверка параметров KDRV_SYS_IO (ядро и хост-тест). Порт = база BAR + смещение, строго внутри BAR, выровнен по ширине. */
int kdrv_io_check(uint64_t a1, uint64_t off, int bar_is_io, uint64_t bar_base, uint64_t bar_size, uint32_t* port, uint32_t* width, uint32_t* op, uint32_t* count) {
    uint32_t bar = (uint32_t)(a1 & 0xFF), w = (uint32_t)((a1 >> 8) & 0xFF), o = (uint32_t)((a1 >> 16) & 0xFF);
    uint64_t n = (a1 >> 24) & 0xFFFFFF;
    if (a1 >> 48) return -10;
    if (bar >= 6 || (w != 1 && w != 2 && w != 4) || o > KDRV_IO_WRITE_MULTI) return -10;
    if (o <= KDRV_IO_WRITE) { if (n > 1) return -10; n = 1; }
    else if (!n || n * w > KDRV_IO_MAX_BYTES) return -10;
    if (!bar_is_io || !bar_size || !bar_base) return -2;                              /* не порты или пустой BAR */
    if (bar_base > 0xFFFF || bar_size > 0x10000 || bar_base + bar_size > 0x10000) return -2;
    if (off >= bar_size || (off % w) || w > bar_size - off) return -10;               /* только внутри BAR своего устройства */
    *port = (uint32_t)(bar_base + off); *width = w; *op = o; *count = (uint32_t)n;
    return 0;
}
