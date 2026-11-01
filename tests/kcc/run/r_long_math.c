long long mulmod(long long a, long long b, long long m) { return (a * b) % m; }
unsigned long long hash(const char *s) { unsigned long long h = 14695981039346656037ULL; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }
int main(void) {
    long long x = 3000000000LL * 3, y = -x / 7, z = x % 1000003;
    unsigned long long h = hash("kira");
    return (int)(mulmod(123456789, 987654321, 1000000007) % 100000) + (int)(y & 0xfff) + (int)z % 1000 + (int)(h >> 59) + (int)(x >> 31) + (int)((unsigned)x);
}
