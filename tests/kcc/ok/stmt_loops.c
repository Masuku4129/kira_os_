int f(int n) { int s = 0; while (n) { s += n--; } do { s++; } while (s < 10); for (;;) { if (s > 20) break; s += 3; continue; } return s; }
