/* SR: два разных 64-битных коэффициента с одинаковыми младшими 32 битами не должны делить накопитель. */
static long long out[4];
int main(void) {
    for (long long i = 0; i < 50; i++) {
        long long a = i * 4294967297LL;
        long long b = i * 8589934593LL;
        long long c = i * -4294967297LL;
        out[0] += a ^ 3; out[1] += b ^ 5; out[2] += c ^ 7;
    }
    return (int)((out[0] ^ out[1] ^ out[2]) & 0xffffff);
}
