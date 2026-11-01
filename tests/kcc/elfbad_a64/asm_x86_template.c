// expect: inline asm template 'rdtsc' is not supported on aarch64
int main(void) {
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (int)lo;
}
