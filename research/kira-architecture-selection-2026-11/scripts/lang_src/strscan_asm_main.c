/* strscan с ядрами на ручном ассемблере (cnt - SSE2, fnv - скалярный) в strscan_kernels.s; остальное (инициализация, цикл) на C -O2. */
static char t[1 << 20];
extern int asm_cnt(const char *s, int n, char c);
extern unsigned asm_fnv(const char *s, int n);
int asm_strscan(void) {
    for (int i = 0; i < (1 << 20); i++) t[i] = (char)('a' + (i * 31 + (i >> 5)) % 26);
    long s = 0;
    for (int r = 0; r < 80; r++) { s += asm_cnt(t, 1 << 20, 'e'); s ^= asm_fnv(t, 1 << 20); t[r] = (char)(t[r] + 1); }
    return (int)(s & 0xffffff);
}
