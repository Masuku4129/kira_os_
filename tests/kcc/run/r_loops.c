int main(void) {
    int s = 0, i, j;
    for (i = 0; i < 10; i++) { if (i == 3) continue; if (i == 8) break; for (j = 0; j < i; j++) s += i * j; }
    i = 5; while (i--) s += i;
    do { s += 3; } while (s % 7);
    for (;;) { s++; if (s > 200) break; }
    return s;
}
