/* [E] Нагрузки для сравнения native / WASM (формат модулей KIRA: userland/wasm/kw.h, импорт "kira", экспорт kmain).
   WL=1 sha256 (свой SHA-256 по 64 КиБ x R), WL=2 sort (пирамидальная сортировка 100000 u32 x R), WL=3 matmul i32 96x96 x R.
   Вывод: контрольная сумма (сравнивается native == wasmtime == интерпретатор KIRA). Одинаковый код для всех целей. */
#include "kw.h"
#ifndef R
#define R 1
#endif
static unsigned ror(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }
static const unsigned K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
static void blk(unsigned* h, const unsigned char* p) {
    unsigned w[64], a, b, c, d, e, f, g, hh;
    for (int i = 0; i < 16; i++) w[i] = (unsigned)p[4*i] << 24 | (unsigned)p[4*i+1] << 16 | (unsigned)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) { unsigned s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) { unsigned S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i], S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj; hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2; }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static unsigned char buf[65536]; static unsigned arr[100000]; static int ma[96 * 96], mb[96 * 96], mc[96 * 96];
static unsigned rs = 12345; static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs; }
static unsigned wl_sha(void) { unsigned h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (unsigned char)(i * 31 + 7);
    for (int r = 0; r < R; r++) for (unsigned o = 0; o < sizeof buf; o += 64) blk(h, buf + o);
    return h[0] ^ h[1] ^ h[2] ^ h[3] ^ h[4] ^ h[5] ^ h[6] ^ h[7]; }
static void sift(unsigned* a, int s, int e) { int r = s; while (2 * r + 1 <= e) { int c = 2 * r + 1; if (c + 1 <= e && a[c] < a[c+1]) c++; if (a[r] < a[c]) { unsigned t = a[r]; a[r] = a[c]; a[c] = t; r = c; } else return; } }
static unsigned wl_sort(void) { unsigned sum = 0, n = 100000;
    for (int r = 0; r < R; r++) { for (unsigned i = 0; i < n; i++) arr[i] = rnd(); for (int s = (int)n / 2 - 1; s >= 0; s--) sift(arr, s, (int)n - 1); for (int e = (int)n - 1; e > 0; e--) { unsigned t = arr[0]; arr[0] = arr[e]; arr[e] = t; sift(arr, 0, e - 1); }
        for (unsigned i = 1; i < n; i++) if (arr[i-1] > arr[i]) return 0xdeadbeef; sum += arr[n/2] ^ arr[0] ^ arr[n-1]; }
    return sum; }
static unsigned wl_mat(void) { unsigned sum = 0; const int N = 96;
    for (int i = 0; i < N * N; i++) { ma[i] = (int)(rnd() & 255) - 128; mb[i] = (int)(rnd() & 255) - 128; }
    for (int r = 0; r < R; r++) { for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) { int s = 0; for (int k = 0; k < N; k++) s += ma[i*N+k] * mb[k*N+j]; mc[i*N+j] = s; } sum += (unsigned)mc[r % (N*N)] ^ (unsigned)mc[N*N-1]; ma[r % (N*N)] ^= 1; }
    return sum; }
#ifdef KW_NATIVE
int main(void) { unsigned v = WL == 1 ? wl_sha() : WL == 2 ? wl_sort() : wl_mat(); kw_say("checksum "); kw_say_x(v); kw_say("\n"); return 0; }
#else
void kmain(void) { unsigned v = WL == 1 ? wl_sha() : WL == 2 ? wl_sort() : wl_mat(); kw_say("checksum "); kw_say_x(v); kw_say("\n"); }
#endif
