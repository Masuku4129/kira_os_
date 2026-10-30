#include <stddef.h>
#include <stdint.h>

// Функции, которые компилятор вправе вызвать сам (копирование структур, обнуление массивов), и общие
// копирования ядра. На всех x86-64 последних лет строковые команды с префиксом rep работают блоками по
// линии кэша (ERMSB/FSRM), это быстрее любого цикла без SSE (ядро собирается без SSE).
// Направление (DF) по ABI на входе в функцию сброшено; memmove назад ставит его сам и сразу сбрасывает.

void* memcpy(void* dst, const void* src, size_t n) {
    void* d = dst;
    __asm__ volatile ("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        __asm__ volatile ("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {                                     // области перекрываются, приёмник выше: копируем с конца
        d += n - 1;
        s += n - 1;
        __asm__ volatile ("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    void* d = dst;
    __asm__ volatile ("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}
