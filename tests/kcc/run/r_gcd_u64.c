unsigned long long gcd(unsigned long long a, unsigned long long b) { return b ? gcd(b, a % b) : a; }
int main(void) {
    unsigned long long x = 600851475143ULL, y = 1ULL << 40, steps = 0, n = 27;
    while (n != 1) { n = (n & 1) ? 3 * n + 1 : n / 2; steps++; }
    return (int)(gcd(x * 6, 1071 * 462) + gcd(y, 1 << 20) % 1000 + steps);
}
