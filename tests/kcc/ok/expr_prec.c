int f(int a, int b, int c) { return a + b * c - (a ^ b) % 3 << 1 | c & 4; }
int g(int x) { return x > 1 && x < 5 || x == 9 ? 1 : 0; }
