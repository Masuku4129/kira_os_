/* Хост-тест формата пакета драйвера: заголовок, запись .rec, сопоставление устройств, враждебные входы. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kdrv_format.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("kdrv_test: FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void mk(kdrv_header_t* h, uint8_t* file, uint32_t elf) {
    memset(h, 0, sizeof *h);
    h->magic = KDRV_MAGIC; h->format = KDRV_FORMAT; h->hdr_size = sizeof *h; strcpy(h->name, "edu");
    h->caps = CAP_MMIO | CAP_IRQ; h->nmatch = 1; h->match[0].vendor = 0x1234; h->match[0].device = 0x11e8; h->elf_size = elf;
    memcpy(file, h, sizeof *h);
}
static uint8_t f[sizeof(kdrv_header_t) + 100];
#define PUT() memcpy(f, &h, sizeof h)

int main(void) {
    kdrv_header_t h, o;
    mk(&h, f, 100);
    CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_OK && !strcmp(o.name, "edu"));
    CHECK(kdrv_name_check(0, 12) == 0);
    CHECK(kdrv_name_check("", 1) == 0);
    CHECK(kdrv_name_check("a", 2) == 1);
    CHECK(kdrv_name_check("driver_12345", 13) == 0); /* 12 characters exceed the ABI limit */
    CHECK(kdrv_name_check("driver-1234", 12) == 1);  /* exactly 11 characters */
    CHECK(kdrv_name_check("bad/name", 9) == 0);
    char unterminated_name[KDRV_NAME_MAX + 1];
    memset(unterminated_name, 'a', sizeof unterminated_name);
    CHECK(kdrv_name_check(unterminated_name, sizeof unterminated_name) == 0);
    CHECK(kdrv_header_check(0, sizeof f, &o) == KDRV_E_INPUT);
    CHECK(kdrv_rec_check(0, sizeof(kdrv_rec_t), 0) == KDRV_E_INPUT);
    CHECK(kdrv_match_dev(0, 0x1234, 0x11e8, 0) == 0);
    CHECK(kdrv_header_check(f, sizeof f, 0) == KDRV_OK);
    CHECK(kdrv_dma_policy_allow(0, 0) == 1);                    /* MMIO/опрос безопасны без DMA */
    CHECK(kdrv_dma_policy_allow(CAP_MMIO, 0) == 1);
    CHECK(kdrv_dma_policy_allow(CAP_DMA, 0) == 0);               /* bus mastering без IOMMU запрещён */
    CHECK(kdrv_dma_policy_allow(CAP_IRQ, 0) == 0);               /* MSI тоже является записью устройства в память */
    CHECK(kdrv_dma_policy_allow(CAP_DMA | CAP_IRQ, 0) == 0);
    CHECK(kdrv_dma_policy_allow(CAP_DMA | CAP_IRQ, 1) == 1);     /* допускается только после изоляции устройства */
    CHECK(kdrv_pci_command_clear_master(0x0007) == 0x0003);   /* clear inherited bus-master permission */
    CHECK(kdrv_pci_command_clear_master(0x0407) == 0x0403);   /* preserve unrelated command bits */
    CHECK(kdrv_pci_busmaster_handoff_safe(0x0003, 0x0003));
    CHECK(!kdrv_pci_busmaster_handoff_safe(0x0007, 0x0003)); /* firmware had enabled DMA */
    CHECK(!kdrv_pci_busmaster_handoff_safe(0x0003, 0x0007)); /* device refused disable */
    uint64_t page_lo = 0, page_hi = 0, physical = 0;
    uint32_t pages = 0;
    CHECK(kdrv_mmio_page_bounds(0xFEC00000, 0x1000, &page_lo, &page_hi) == 0 &&
          page_lo == 0xFEC00000 && page_hi == 0xFEC01000);
    CHECK(kdrv_mmio_page_bounds(0xFEC00000, 0x1001, &page_lo, &page_hi) == 0 &&
          page_hi == 0xFEC02000);
    CHECK(kdrv_mmio_page_bounds(0, 0x1000, &page_lo, &page_hi) != 0);
    CHECK(kdrv_mmio_page_bounds(UINT64_MAX - 0xFFF, 0x2000, &page_lo, &page_hi) != 0);
    CHECK(kdrv_mmio_page_bounds(UINT64_MAX - 0x1FFF, 0x1001, &page_lo, &page_hi) != 0);
    CHECK(kdrv_mmio_map_bounds(0xFEC00000, 0x1001, 0x1000, 1, &physical, &pages) == 0 &&
          physical == 0xFEC01000 && pages == 1);
    CHECK(kdrv_mmio_map_bounds(0xFEC00000, 0x1001, 0x1000, 2, &physical, &pages) != 0);
    CHECK(kdrv_mmio_map_bounds(0xFEC00000, 0x1000, 1, 1, &physical, &pages) != 0);
    CHECK(kdrv_mmio_map_bounds(0xFEC00000, 0x1000, 0, 0, &physical, &pages) != 0);
    CHECK(kdrv_mmio_map_bounds(UINT64_MAX - 0xFFF, 0x2000, 0, 0x1000, &physical, &pages) != 0);
    CHECK(kdrv_header_check(f, sizeof f - 1, &o) == KDRV_E_SIZE);
    CHECK(kdrv_header_check(f, 10, &o) == KDRV_E_SHORT);
    h.magic ^= 1; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MAGIC); h.magic ^= 1;
    h.caps = CAP_NET; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_CAPS);   /* права обычных программ - нельзя */
    h.caps = CAP_FS | CAP_MMIO; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_CAPS);
    h.caps = CAP_MMIO; strcpy(h.name, "../x"); PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_NAME);
    strcpy(h.name, "Edu"); PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_NAME);
    memset(h.name, 'a', sizeof h.name); PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_NAME);   /* без нуля */
    strcpy(h.name, "abcdefghijkl"); PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_NAME);        /* 12 знаков */
    strcpy(h.name, "edu"); h.nmatch = 0; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MATCH);
    h.nmatch = 9; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MATCH);
    h.nmatch = 1; h.match[0].vendor = h.match[0].device = KDRV_ANY; h.match[0].cls_mask = 0; PUT();
    CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MATCH);                                  /* подошло бы любое устройство */
    h.match[0].vendor = KDRV_ANY; h.match[0].device = 5; h.match[0].cls_mask = 0xFF0000; PUT();
    CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MATCH);                                  /* номер без изготовителя */
    h.match[0].device = KDRV_ANY; h.match[0].cls = 0x020000; h.match[0].cls_mask = 0xFF0000; PUT();
    CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_OK);
    CHECK(kdrv_match_dev(&o, 0x10EC, 0x8139, 0x020000) == 1 && kdrv_match_dev(&o, 0x10EC, 0x8139, 0x010000) == 0);
    h.elf_size = 0; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_SIZE);
    h.elf_size = 100; h.match[0].cls_mask = 0xFF000000; PUT(); CHECK(kdrv_header_check(f, sizeof f, &o) == KDRV_E_MATCH);
    mk(&h, f, 100); kdrv_header_check(f, sizeof f, &o);                                         /* точное соответствие vendor:device */
    CHECK(kdrv_match_dev(&o, 0x1234, 0x11e8, 0x0) && !kdrv_match_dev(&o, 0x1234, 0x11e9, 0) && !kdrv_match_dev(&o, 0x1235, 0x11e8, 0));
    kdrv_rec_t r; memset(&r, 0, sizeof r);
    r.magic = KREC_MAGIC; r.format = KDRV_FORMAT; r.rec_size = sizeof r; r.confirmed = KREC_CONFIRMED; r.caps_ok = CAP_MMIO; strcpy(r.source, "x");
    kdrv_rec_t ro;
    CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_OK);
    r.confirmed = 0; CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_UNCONFIRMED); r.confirmed = KREC_CONFIRMED;
    r.caps_ok = CAP_GFX; CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_CAPS); r.caps_ok = CAP_MMIO;
    memset(r.source, 'a', sizeof r.source); CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_FORMAT); r.source[0] = 0;
    CHECK(kdrv_rec_check(&r, sizeof r - 1, &ro) == KDRV_E_SHORT);
    CHECK(kdrv_rec_check(&r, sizeof r + 1, &ro) == KDRV_E_FORMAT);
    srand(7);                                                                                   /* случайные искажения */
    for (int i = 0; i < 20000; i++) {
        mk(&h, f, 100);
        for (int k = 0; k < 1 + rand() % 6; k++) f[rand() % sizeof(kdrv_header_t)] ^= (uint8_t)(1 + rand() % 255);
        if (kdrv_header_check(f, sizeof f, &o) == KDRV_OK) CHECK(!(o.caps & ~(uint32_t)CAP_DRIVER_) && o.nmatch >= 1 && o.nmatch <= KDRV_MATCH_MAX);
    }
    printf(fails ? "kdrv_test: FAIL (%d)\n" : "kdrv_test: PASS\n", fails);
    return fails != 0;
}
