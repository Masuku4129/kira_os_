/* Копия аккумулятора перед встроенным вызовом (s += f(x) читает s до вызова): после встраивания jmp на соседнюю метку не должен
 * мешать распространению копий; результат должен совпасть с тем, что даёт вызов без встраивания (рекурсия, адрес функции). */
static int sq(int x) { return x * x + 1; }
static long wide(int x) { if (x & 1) return (long)x * 3; return (long)x - 7; }
static int sidefx;
static int bump(int x) { sidefx += x; return sidefx; }
static int (*volatile fp)(int) = sq;
static long sum1(int n) { long s = 0; for (int i = 0; i < n; i++) s += sq(i & 255); return s; }
static long sum2(int n) { long s = 0; for (int i = 0; i < n; i++) s += wide(i); return s; }
static long sum3(int n) { long s = 1; for (int i = 0; i < n; i++) s += bump(i) - s; return s; }     /* вызов меняет то, что читается после */
static long sum4(int n) { long s = 0; for (int i = 0; i < n; i++) s = s * 3 + sq(i) - (s >> 2); return s; }
static long sum5(int n) { long s = 0; for (int i = 0; i < n; i++) s += fp(i); return s; }
static int tern(int n) { int s = 0; for (int i = 0; i < n; i++) s += (i & 1) ? sq(i) : wide(i); return s; }
int main(void) {
    long acc = sum1(1000) + sum2(777) + sum3(50) + sum4(40) + sum5(300) + tern(500);
    return (int)(acc & 0x7fffff);
}
