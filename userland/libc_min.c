/* Минимум libc для программ из нескольких файлов: компилятор сам вызывает memcpy/memset при копировании структур. */
typedef unsigned long size_t;
void* memcpy(void* d, const void* s, size_t n) { unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d; }
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = d; const unsigned char* b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, size_t n) { unsigned char* a = d; while (n--) *a++ = (unsigned char)c; return d; }
int memcmp(const void* x, const void* y, size_t n) {
    const unsigned char* a = x; const unsigned char* b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
