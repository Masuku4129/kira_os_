/* Воспроизведение находки F1: kbi_from_fdt() -> fdt_tree_valid() -> fdt_valid() читает 40-байтовый заголовок FDT, не проверив, что available >= 40
   (max_len сравнивается только с total из самого заголовка). Вход: файл (4 байта d00dfeed достаточно). Буфер выделен точно по размеру файла. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "bootinfo.h"
int main(int c, char** v) { FILE* f = fopen(v[1], "rb"); uint8_t* b = malloc(64); size_t n = fread(b, 1, 64, f); fclose(f); uint8_t* e = malloc(n); for (size_t i = 0; i < n; i++) e[i] = b[i];
    static uint8_t out[1 << 16]; uint32_t os; int r = kbi_from_fdt(e, (uint32_t)n, 0x40000000, 0x40080000, 0x100000, out, sizeof out, &os); printf("kbi_from_fdt(%zu байт) = %d\n", n, r); return 0; }
