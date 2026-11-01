/* Библиотека программ WASM: то, что компилятор может вызвать сам. */
#include "kw.h"
void* memcpy(void* d, const void* s, unsigned long n) { unsigned char* a = d; const unsigned char* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { unsigned char* a = d; while (n--) *a++ = (unsigned char)c; return d; }
