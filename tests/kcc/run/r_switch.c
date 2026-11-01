int f(int x) { int r = 0; switch (x) { case 1: r += 1; case 2: r += 2; break; case 5: r = 50; break; case -3: r = 30; default: r += 100; } return r; }
int main(void) { int s = 0; for (int i = -4; i < 8; i++) s = s * 3 + f(i); return s & 0x7fffffff; }
