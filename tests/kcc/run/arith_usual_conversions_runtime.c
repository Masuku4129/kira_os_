int main(void) {
    if (!(-1L < 1U)) return 1;
    if (-1LL < 1UL) return 2;
    if (1U + -1L != 0L) return 3;
    if (1UL + -1LL != 0ULL) return 4;
    if (sizeof(1U + 1L) != sizeof(long)) return 5;
    if (sizeof(1UL + 1LL) != sizeof(unsigned long long)) return 6;
    return 0;
}
