/* Образец для tests/kreloc_test.sh: все виды слов с абсолютными адресами, которые бывают в ядре. */
typedef unsigned long u64;
extern char ABSVAL[];                         /* --defsym: абсолютный символ, от сдвига не зависит */
static char buf[64];
char gbuf[32] = "data";
const char* const names[] = { "alpha", "beta", gbuf };            /* R_X86_64_64 в .data.rel.ro / .data */
int fa(int x) { return x + 1; }
int fb(int x) { return x * 2; }
int (*const table[])(int) = { fa, fb };                           /* указатели на функции */
u64 addr32(void) { return (u64)(unsigned)(u64)&buf[3]; }          /* mov $buf+3, %eax: R_X86_64_32 */
u64 addr_abs(void) { return (u64)(unsigned)(u64)ABSVAL; }        /* то же против абсолютного символа: сдвигать нельзя */
int sw(int x) {                                                   /* таблица переходов switch: R_X86_64_64 в .rodata */
    switch (x) { case 0: return 10; case 1: return 21; case 2: return 32; case 3: return 43; case 4: return 54; case 5: return 65; case 6: return 76; default: return 0; }
}
u64 arr32(int i) { return ((const unsigned*)(u64)(unsigned)(u64)buf)[i]; }   /* buf(,%rax,4): R_X86_64_32S */
void _start(void) { buf[0] = (char)(addr32() + addr_abs() + sw(3) + (u64)names[0] + table[1](2) + arr32(1)); for (;;) {} }
