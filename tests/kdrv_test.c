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

static int hexbyte(const char* s) {
    unsigned a = s[0] <= '9' ? (unsigned)(s[0] - '0') : (unsigned)(s[0] - 'a' + 10);
    unsigned b = s[1] <= '9' ? (unsigned)(s[1] - '0') : (unsigned)(s[1] - 'a' + 10);
    return (int)((a << 4) | b);
}
static void unhex(uint8_t* out, const char* in, unsigned n) {
    for (unsigned i = 0; i < n; i++) out[i] = (uint8_t)hexbyte(in + i * 2);
}

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
    /* RFC 8032 test vector 2: message 0x72, checked through the KDRV signature envelope. */
    static const char pub_hex[] = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
    static const char sig_hex[] =
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
    uint8_t pub[32], message = 0x72;
    kdrv_sig_t sig; memset(&sig, 0, sizeof sig);
    unhex(pub, pub_hex, sizeof pub);
    unhex(sig.signature, sig_hex, sizeof sig.signature);
    sig.magic = KDRV_SIG_MAGIC; sig.format = KDRV_SIG_FORMAT; sig.sig_size = sizeof sig.signature;
    CHECK(kdrv_sig_verify_with_key(&message, 1, &sig, pub) == KDRV_OK);
    message ^= 1;
    CHECK(kdrv_sig_verify_with_key(&message, 1, &sig, pub) == KDRV_E_SIGNATURE);
    sig.magic = 0;
    CHECK(kdrv_sig_verify_with_key(&message, 1, &sig, pub) == KDRV_E_SIGNATURE);
    sig.magic = KDRV_SIG_MAGIC;
    memset(sig.key_id, '0', sizeof sig.key_id);
    CHECK(kdrv_sig_check(&message, 1, &sig, sizeof sig) == KDRV_E_SIGNATURE);
    printf(fails ? "kdrv_test: FAIL (%d)\n" : "kdrv_test: PASS\n", fails);
    return fails != 0;
}
