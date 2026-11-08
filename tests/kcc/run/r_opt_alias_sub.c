/* Оптимизатор: приёмник совпадает с правым операндом, вычитание, сдвиги, умножение на константы. */
static int f(int a, int b) { int t = a; a = b - a; b = t - b; return a * 1000 + b; }
int main(void) {
    int a = 50, b = 8, s = 0;
    for (int i = 0; i < 20; i++) { s = i - s; a = a - b; b = b - a; s += a >> 2; s ^= b << 3; s += i * 8 + i * 5 + i * 1; }
    unsigned u = 3000000000u, v = 7;
    u = u * 16 + v * 3;
    long long big = 1;
    for (int i = 0; i < 40; i++) big = big * 3 + (big >> 7);
    return (f(a, b) + s + (int)(u & 0xffff) + (int)(big & 0xffff)) & 0xffffff;
}
