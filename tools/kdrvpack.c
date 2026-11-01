/* kdrvpack: пакет драйвера Киры на хосте (при сборке образа и для проверок). Из ELF драйвера делает /drv/ИМЯ.kdrv (kdrv_format.h) и
   /drv/ИМЯ.rec с хешами и подтверждением. На самой Кире то же делает установка драйвера после явного согласия пользователя;
   здесь «согласие» - ключ --confirm (сборка образа с тестовыми драйверами: подтверждает тот, кто собирает образ).
     kdrvpack ИМЯ драйвер.elf выход.kdrv выход.rec --pci VVVV:DDDD [--pci ...] [--class CCSSPP:МАСКА] --caps mmio,irq,dma,pcidev,netdev
              [--version N] [--source ТЕКСТ] [--src-file ФАЙЛ] [--confirm] [--corrupt]
   --corrupt портит хеш в .rec (проверка отказа загрузки). Без --confirm запись не подтверждена и драйвер не запустится. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kdrv_format.h"
#include "immune.h"

static uint8_t* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)l + 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fprintf(stderr, "kdrvpack: чтение %s\n", p); exit(1); }
    fclose(f); *n = (size_t)l;
    return b;
}

static uint32_t caps_of(const char* s) {
    uint32_t c = 0;
    char buf[128]; snprintf(buf, sizeof buf, "%s", s);
    for (char* t = strtok(buf, ","); t; t = strtok(0, ",")) {
        if (!strcmp(t, "mmio")) c |= CAP_MMIO; else if (!strcmp(t, "irq")) c |= CAP_IRQ; else if (!strcmp(t, "dma")) c |= CAP_DMA;
        else if (!strcmp(t, "pcidev")) c |= CAP_PCIDEV; else if (!strcmp(t, "netdev")) c |= CAP_NETDEV;
        else { fprintf(stderr, "kdrvpack: неизвестное право '%s'\n", t); exit(2); }
    }
    return c;
}

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "использование: kdrvpack ИМЯ драйвер.elf выход.kdrv выход.rec --pci VVVV:DDDD --caps mmio,irq,dma,pcidev,netdev [--confirm]\n"); return 2; }
    kdrv_header_t h; memset(&h, 0, sizeof h);
    kdrv_rec_t r; memset(&r, 0, sizeof r);
    h.magic = KDRV_MAGIC; h.format = KDRV_FORMAT; h.hdr_size = sizeof h; h.version = 1;
    snprintf(h.name, sizeof h.name, "%s", argv[1]);
    int confirm = 0, corrupt = 0;
    const char* source = "tools/kdrvpack (сборка образа)"; const char* srcfile = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--pci") && i + 1 < argc && h.nmatch < KDRV_MATCH_MAX) {
            unsigned v, d; if (sscanf(argv[++i], "%x:%x", &v, &d) != 2) return 2;
            kdrv_match_t* m = &h.match[h.nmatch++]; m->vendor = (uint16_t)v; m->device = (uint16_t)d;
        } else if (!strcmp(argv[i], "--class") && i + 1 < argc && h.nmatch < KDRV_MATCH_MAX) {
            unsigned c, mk; if (sscanf(argv[++i], "%x:%x", &c, &mk) != 2) return 2;
            kdrv_match_t* m = &h.match[h.nmatch++]; m->vendor = m->device = KDRV_ANY; m->cls = c; m->cls_mask = mk;
        } else if (!strcmp(argv[i], "--caps") && i + 1 < argc) h.caps = caps_of(argv[++i]);
        else if (!strcmp(argv[i], "--version") && i + 1 < argc) h.version = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--source") && i + 1 < argc) source = argv[++i];
        else if (!strcmp(argv[i], "--src-file") && i + 1 < argc) srcfile = argv[++i];
        else if (!strcmp(argv[i], "--confirm")) confirm = 1;
        else if (!strcmp(argv[i], "--corrupt")) corrupt = 1;
        else { fprintf(stderr, "kdrvpack: непонятный аргумент %s\n", argv[i]); return 2; }
    }
    size_t en; uint8_t* elf = slurp(argv[2], &en);
    h.elf_size = (uint32_t)en;
    uint8_t* pkg = malloc(sizeof h + en);
    memcpy(pkg, &h, sizeof h); memcpy(pkg + sizeof h, elf, en);
    int e = kdrv_header_check(pkg, (uint32_t)(sizeof h + en), 0);
    if (e) { fprintf(stderr, "kdrvpack: %s\n", kdrv_strerror(e)); return 1; }
    r.magic = KREC_MAGIC; r.format = KDRV_FORMAT; r.rec_size = sizeof r;
    sha256(elf, en, r.elf_sha); sha256(pkg, sizeof h + en, r.pkg_sha);
    if (srcfile) { size_t sn; uint8_t* sb = slurp(srcfile, &sn); sha256(sb, sn, r.src_sha); }
    r.caps_ok = h.caps; r.confirmed = confirm ? KREC_CONFIRMED : 0; r.date = (uint64_t)time(0);
    snprintf(r.source, sizeof r.source, "%s", source);
    if (corrupt) r.elf_sha[0] ^= 0x5A;
    FILE* f = fopen(argv[3], "wb"); if (!f || fwrite(pkg, 1, sizeof h + en, f) != sizeof h + en) { perror(argv[3]); return 1; } fclose(f);
    f = fopen(argv[4], "wb"); if (!f || fwrite(&r, 1, sizeof r, f) != sizeof r) { perror(argv[4]); return 1; } fclose(f);
    printf("kdrvpack: %s: %zu байт ELF, права 0x%x, устройств в списке %u%s%s\n", h.name, en, h.caps, h.nmatch, confirm ? ", подтверждён" : ", НЕ подтверждён", corrupt ? ", хеш испорчен нарочно" : "");
    return 0;
}
