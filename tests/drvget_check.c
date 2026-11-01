// Проверка пакета, который записал drvget (хост-тест tests/drvget_e2e.sh): заголовок, запись, SHA-256 образа и пакета, elf_parse ядра.
#include "../kdrv_format.h"
#include "../elf.h"
#include "../immune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char* slurp(const char* p, long* n) {
    FILE* f = fopen(p, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* b = malloc((size_t)*n + 1); if (fread(b, 1, (size_t)*n, f) != (size_t)*n) return 0; fclose(f); return b;
}
int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: drvget_check dir name want-caps-hex\n"); return 2; }
    char p1[300], p2[300]; long n1, n2;
    snprintf(p1, sizeof p1, "%s/%s.kdrv", argv[1], argv[2]); snprintf(p2, sizeof p2, "%s/%s.rec", argv[1], argv[2]);
    unsigned char* k = slurp(p1, &n1); unsigned char* r = slurp(p2, &n2);
    if (!k || !r) { printf("NOFILES\n"); return 1; }
    kdrv_header_t h; kdrv_rec_t rec;
    int e = kdrv_header_check(k, (uint32_t)n1, &h); if (e) { printf("BAD header %d\n", e); return 1; }
    e = kdrv_rec_check(r, (uint32_t)n2, &rec); if (e) { printf("BAD rec %d\n", e); return 1; }
    uint8_t d1[32], d2[32];
    sha256(k + sizeof h, h.elf_size, d1); sha256(k, (size_t)n1, d2);
    if (memcmp(d1, rec.elf_sha, 32) || memcmp(d2, rec.pkg_sha, 32)) { printf("BAD hashes\n"); return 1; }
    uint32_t want = (uint32_t)strtoul(argv[3], 0, 16);
    if (h.caps != want || rec.caps_ok != want) { printf("BAD caps %x/%x\n", h.caps, rec.caps_ok); return 1; }
    elf_info_t info;
    e = elf_parse(k + sizeof h, h.elf_size, 0x8000000000ULL, 0x800000000000ULL, 4096, &info); if (e) { printf("BAD elf %d\n", e); return 1; }
    if (strcmp(h.name, argv[2])) { printf("BAD name\n"); return 1; }
    if (h.nmatch < 1 || h.match[0].vendor != 0x10ec || h.match[0].device != 0x8139) { printf("BAD match\n"); return 1; }
    int zero = 1; for (int i = 0; i < 32; i++) if (rec.src_sha[i]) zero = 0;
    if (zero || rec.source[0] != 'h') { printf("BAD src\n"); return 1; }
    printf("OK %s v%u caps=%x elf=%u bytes source=%s\n", h.name, h.version, h.caps, h.elf_size, rec.source);
    return 0;
}
