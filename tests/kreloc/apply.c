/* apply образ.bin таблица.bin slide выход.bin: kreloc_apply на файловой части образа (для tests/kreloc_test.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kaslr.h"

static unsigned char* rd(const char* p, long* n) {
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    unsigned char* b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    long ni, nt;
    unsigned char* img = rd(argv[1], &ni);
    unsigned char* tbl = rd(argv[2], &nt);
    long long r = kreloc_apply(img, (uint32_t)ni, tbl, (uint32_t)nt, strtoull(argv[3], 0, 0));
    if (r < 0) { fprintf(stderr, "kreloc_apply failed\n"); return 1; }
    printf("applied %lld words\n", r);
    FILE* f = fopen(argv[4], "wb");
    fwrite(img, 1, (size_t)ni, f);
    fclose(f);
    return 0;
}
