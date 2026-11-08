/* Находки аудита CG-20261108-012: атомики не пересекаются LVN, 64-битные коэффициенты в SR, длинные цепочки копий в nonorm. */
static int g;
static long long out[8];
int main(void) {
    long long acc = 0;
    for (int i = 0; i < 6; i++) {
        int a = g; __atomic_store_n(&g, a + 7, __ATOMIC_SEQ_CST); int b = g; acc += b - a;
        int c = __atomic_load_n(&g, __ATOMIC_SEQ_CST); int d = __atomic_exchange_n(&g, c + 1, __ATOMIC_SEQ_CST); int e = g; acc += c * 3 + d + e;
        int f = __atomic_load_n(&g, __ATOMIC_SEQ_CST); __atomic_fetch_add(&g, 5, __ATOMIC_SEQ_CST); int h = g; acc += h - f;
    }
    return (int)(acc & 0xffffff);
}
