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
    CHECK(kdrv_dma_policy_allow(0, 0) == 1);                    /* MMIO/опрос безопасны без DMA */
    CHECK(kdrv_dma_policy_allow(CAP_MMIO, 0) == 1);
    CHECK(kdrv_dma_policy_allow(CAP_DMA, 0) == 0);               /* bus mastering без IOMMU запрещён */
    CHECK(kdrv_dma_policy_allow(CAP_IRQ, 0) == 0);               /* MSI тоже является записью устройства в память */
    CHECK(kdrv_dma_policy_allow(CAP_DMA | CAP_IRQ, 0) == 0);
    CHECK(kdrv_dma_policy_allow(CAP_DMA | CAP_IRQ, 1) == 1);     /* допускается только после изоляции устройства */
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
    CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_OK && !kdrv_rec_publisher_required(&ro));
    r.confirmed = KREC_CONFIRMED_PUBLISHER; CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_OK && kdrv_rec_publisher_required(&ro));
    r.confirmed = 0; CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_UNCONFIRMED); r.confirmed = KREC_CONFIRMED;
    r.caps_ok = CAP_GFX; CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_CAPS); r.caps_ok = CAP_MMIO;
    memset(r.source, 'a', sizeof r.source); CHECK(kdrv_rec_check(&r, sizeof r, &ro) == KDRV_E_FORMAT); r.source[0] = 0;
    CHECK(kdrv_rec_check(&r, sizeof r - 1, &ro) == KDRV_E_SHORT);
    CHECK(kdrv_rec_check(&r, sizeof r + 1, &ro) == KDRV_E_FORMAT);
    {   /* KDRV_SYS_IO: границы I/O BAR */
        uint32_t port, w, op, n;
        #define IOC(a1, off, io, base, size) kdrv_io_check(a1, off, io, base, size, &port, &w, &op, &n)
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0), 0x0F, 1, 0xC000, 0x20) == 0 && port == 0xC00F && w == 1 && n == 1);
        CHECK(IOC(kdrv_io_pack(0, 4, KDRV_IO_WRITE, 0), 0x1C, 1, 0xC000, 0x20) == 0);
        CHECK(IOC(kdrv_io_pack(0, 4, KDRV_IO_WRITE, 0), 0x1D, 1, 0xC000, 0x20) == -10);       /* невыровнено */
        CHECK(IOC(kdrv_io_pack(0, 4, KDRV_IO_READ, 0), 0x20, 1, 0xC000, 0x20) == -10);        /* за концом */
        CHECK(IOC(kdrv_io_pack(0, 2, KDRV_IO_READ, 0), 0x1F, 1, 0xC000, 0x20) == -10);
        CHECK(IOC(kdrv_io_pack(0, 3, KDRV_IO_READ, 0), 0, 1, 0xC000, 0x20) == -10);           /* ширина */
        CHECK(IOC(kdrv_io_pack(0, 1, 9, 0), 0, 1, 0xC000, 0x20) == -10);                      /* операция */
        CHECK(IOC(kdrv_io_pack(6, 1, KDRV_IO_READ, 0), 0, 1, 0xC000, 0x20) == -10);           /* номер BAR */
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0), 0, 0, 0xFEBC0000u, 0x1000) == -2);     /* память, не порты */
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0), 0, 1, 0xC000, 0) == -2);               /* пустой BAR */
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0), 0, 1, 0xFFF0, 0x20) == -2);            /* BAR выходит за 64 КБ портов */
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0), 0, 1, 0x1FFF0, 0x10) == -2);
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 3), 0, 1, 0xC000, 0x20) == -10);          /* число для одиночной операции */
        CHECK(IOC(kdrv_io_pack(0, 4, KDRV_IO_READ_MULTI, 1024), 0, 1, 0xC000, 0x20) == 0 && n == 1024);
        CHECK(IOC(kdrv_io_pack(0, 4, KDRV_IO_READ_MULTI, 1025), 0, 1, 0xC000, 0x20) == -10);  /* больше 4096 байт */
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_WRITE_MULTI, 0), 0, 1, 0xC000, 0x20) == -10);
        CHECK(IOC(kdrv_io_pack(0, 1, KDRV_IO_READ, 0) | (1ull << 50), 0, 1, 0xC000, 0x20) == -10);
    }
    srand(7);                                                                                   /* случайные искажения */
    for (int i = 0; i < 20000; i++) {
        mk(&h, f, 100);
        for (int k = 0; k < 1 + rand() % 6; k++) f[rand() % sizeof(kdrv_header_t)] ^= (uint8_t)(1 + rand() % 255);
        if (kdrv_header_check(f, sizeof f, &o) == KDRV_OK) CHECK(!(o.caps & ~(uint32_t)CAP_DRIVER_) && o.nmatch >= 1 && o.nmatch <= KDRV_MATCH_MAX);
    }
    printf(fails ? "kdrv_test: FAIL (%d)\n" : "kdrv_test: PASS\n", fails);
    return fails != 0;
}
