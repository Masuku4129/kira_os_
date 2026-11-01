int f(int a, int b, int c, int d, int e, int g, int h, int i, int j) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * g + 7 * h + 8 * i + 9 * j; }
long mix(char a, short b, long c, unsigned char d, unsigned e) { return a + b + c + d + e; }
int main(void) { return f(1, 2, 3, 4, 5, 6, 7, 8, 9) + (int)mix(-1, -2, 1000000000000L, 255, 4000000000u) % 1000; }
