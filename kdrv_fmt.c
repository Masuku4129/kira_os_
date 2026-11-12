/* Разбор и проверка пакета драйвера (kdrv_format.h). Чистые функции без обращений к ядру: их используют ядро (kdrv.c),
   хост-инструмент tools/kdrvpack и хост-тест. Входу не доверяют: каждое поле проверяется, имя - символы и длина. */
#include "kdrv_format.h"

static void cpy(void* d, const void* s, uint32_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }

int kdrv_name_check(const char* n, uint32_t cap) {
    if (!n || !cap) return 0;
    for (uint32_t i = 0; i < cap; i++) {
        char c = n[i];
        if (!c) return i >= 1 && i <= KDRV_NAME_MAX;
        if (i >= KDRV_NAME_MAX ||
            !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 0;                                      /* нет завершающего нуля в пределах буфера */
}

static int name_ok(const char* n, unsigned cap) {
    return kdrv_name_check(n, cap);                /* единая проверка имён для пакета и менеджера */
}

int kdrv_header_check(const void* file, uint32_t len, kdrv_header_t* out) {
    kdrv_header_t h;
    if (!file) return KDRV_E_INPUT;
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
    if (!rec) return KDRV_E_INPUT;
    if (len < sizeof r) return KDRV_E_SHORT;
    cpy(&r, rec, sizeof r);
    if (r.magic != KREC_MAGIC) return KDRV_E_MAGIC;
    if (r.format != KDRV_FORMAT || r.rec_size != sizeof r || len != sizeof r) return KDRV_E_FORMAT;
    unsigned z = 0;
    while (z < sizeof r.source && r.source[z]) z++;
    if (z == sizeof r.source) return KDRV_E_FORMAT;     /* строка без нуля */
    if (r.confirmed != KREC_CONFIRMED) return KDRV_E_UNCONFIRMED;
    if (r.caps_ok & ~(uint32_t)CAP_DRIVER_) return KDRV_E_CAPS;
    if (out) *out = r;
    return KDRV_OK;
}

int kdrv_match_dev(const kdrv_header_t* h, uint16_t vendor, uint16_t device, uint32_t cls) {
    if (!h || !h->nmatch || h->nmatch > KDRV_MATCH_MAX) return 0;
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

/* PCI Command bit 2 enables device-initiated memory transactions. An untrusted
   Ring 3 driver must never inherit that permission from firmware or a prior owner. */
uint16_t kdrv_pci_command_clear_master(uint16_t command) {
    return (uint16_t)(command & ~(1u << 2));
}

uint16_t kdrv_pci_command_for_unisolated(uint16_t command) {
    return (uint16_t)(kdrv_pci_command_clear_master(command) | (1u << 10));
}

int kdrv_pci_busmaster_handoff_safe(uint16_t before, uint16_t after) {
    const uint16_t bus_master = 1u << 2;
    return ((before | after) & bus_master) == 0;
}

int kdrv_pci_command_quiesced(uint16_t command) {
    const uint16_t bus_master = 1u << 2;
    const uint16_t intx_disable = 1u << 10;
    return !(command & bus_master) && (command & intx_disable) != 0;
}

int kdrv_mmio_page_bounds(uint64_t base, uint64_t size, uint64_t* page_lo, uint64_t* page_hi) {
    if (!base || !size || !page_lo || !page_hi || base > UINT64_MAX - size) return -1;
    uint64_t end = base + size;
    if (end > UINT64_MAX - 4095u) return -1;
    uint64_t lo = base & ~4095ull;
    uint64_t hi = (end + 4095u) & ~4095ull;
    if (hi <= lo) return -1;
    *page_lo = lo;
    *page_hi = hi;
    return 0;
}

int kdrv_mmio_map_bounds(uint64_t base, uint64_t size, uint64_t offset, uint64_t length,
                         uint64_t* physical, uint32_t* pages) {
    uint64_t lo, hi;
    if (!physical || !pages || !length || length > KDRV_MAX_MMIO_BYTES ||
        (base & 4095u) || (offset & 4095u) ||
        kdrv_mmio_page_bounds(base, size, &lo, &hi) != 0 ||
        offset >= size || length > size - offset || base > UINT64_MAX - offset) return -1;
    (void)lo;
    uint64_t count = (length + 4095u) / 4096u;
    uint64_t pa = base + offset;
    uint64_t bytes = count * 4096u;
    if (!count || count > UINT32_MAX || pa > UINT64_MAX - bytes || pa + bytes > hi) return -1;
    *physical = pa;
    *pages = (uint32_t)count;
    return 0;
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
    case KDRV_E_INPUT: return "отсутствует входной буфер";
    default: return "?";
    }
}
