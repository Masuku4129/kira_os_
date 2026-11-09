// C++-эквиваленты 4 нагрузок, freestanding-стиль (без исключений/RTTI/libstdc++), C++17: constexpr, шаблоны, enum-классов нет.
// Тот же автор и стиль, что у C-исходников (см. ограничения в language-toolchain-matrix.md).
typedef unsigned u32; typedef unsigned char u8; typedef long long i64;

// ---------- sha256 ----------
#ifdef W_sha256
static constexpr u32 K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static constexpr u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
static void blk(u32 *st, const u8 *p) {
    u32 w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)p[4*i] << 24 | (u32)p[4*i+1] << 16 | (u32)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) { u32 s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3), s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
    u32 a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for (int i = 0; i < 64; i++) { u32 t1 = h + (ror(e,6) ^ ror(e,11) ^ ror(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i], t2 = (ror(a,2) ^ ror(a,13) ^ ror(a,22)) + ((a & b) ^ (a & c) ^ (b & c)); h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}
static u8 buf[64*1024];
extern "C" int cpp_sha256(void) {
    u32 st[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (u8)(i*7+3);
    for (int r = 0; r < 60; r++) for (int o = 0; o < (int)sizeof buf; o += 64) blk(st, buf + o);
    return (int)((st[0] ^ st[3] ^ st[7]) & 0xffffff);
}
#endif

// ---------- sort (шаблон) ----------
#ifdef W_sort
template <typename T> static void qs(T *a, int lo, int hi) {
    while (lo < hi) {
        T p = a[(lo + hi) / 2]; int i = lo, j = hi;
        while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { T t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
        if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
    }
}
static int v[200000];
extern "C" int cpp_sort(void) {
    u32 x = 12345; i64 chk = 0;
    for (int r = 0; r < 20; r++) {
        for (int i = 0; i < 200000; i++) { x = x * 1664525u + 1013904223u; v[i] = (int)(x >> 8); }
        qs<int>(v, 0, 199999);
        for (int i = 1; i < 200000; i++) if (v[i-1] > v[i]) return 1;
        chk += v[100000];
    }
    return (int)(chk & 0xffffff);
}
#endif

// ---------- matmul int ----------
#ifdef W_matmul
static int A[120][120], B[120][120], C[120][120];
extern "C" int cpp_matmul(void) {
    for (int i = 0; i < 120; i++) for (int j = 0; j < 120; j++) { A[i][j] = i + j; B[i][j] = i - j; }
    i64 t = 0;
    for (int r = 0; r < 40; r++) { for (int i = 0; i < 120; i++) for (int j = 0; j < 120; j++) { int s = 0; for (int k = 0; k < 120; k++) s += A[i][k] * B[k][j]; C[i][j] = s; } t += C[r][r]; }
    return (int)(t & 0xffffff);
}
#endif

// ---------- strscan ----------
#ifdef W_strscan
static char tt[1 << 20];
static int cnt(const char *s, int n, char c) { int k = 0; for (int i = 0; i < n; i++) if (s[i] == c) k++; return k; }
static u32 fnv(const char *s, int n) { u32 h = 2166136261u; for (int i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; } return h; }
extern "C" int cpp_strscan(void) {
    for (int i = 0; i < (1 << 20); i++) tt[i] = (char)('a' + (i * 31 + (i >> 5)) % 26);
    i64 s = 0;
    for (int r = 0; r < 80; r++) { s += cnt(tt, 1 << 20, 'e'); s ^= fnv(tt, 1 << 20); tt[r] = (char)(tt[r] + 1); }
    return (int)(s & 0xffffff);
}
#endif

