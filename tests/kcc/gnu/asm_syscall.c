typedef unsigned long long u64;
typedef long long i64;
static inline i64 ksys(u64 n, u64 a, u64 b, u64 c) {
    i64 r;
    __asm__ volatile ("syscall" : "=a"(r), "+D"(a), "+S"(b), "+d"(c) : "a"(n) : "rcx", "r11", "r8", "r9", "r10", "memory");
    return r;
}
i64 f(void) { return ksys(1, 2, 3, 4); }
