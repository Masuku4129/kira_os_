/* CAS и дерево Меркла: векторы RFC 6962 (Certificate Transparency), пути включения для всех листьев и размеров, подмена данных,
   дедупликация, сборка мусора, файлы кусками. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cas.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)
static void unhex(const char* h, uint8_t* o) { for (; h[0] && h[1]; h += 2, o++) { unsigned v; sscanf(h, "%2x", &v); *o = (uint8_t)v; } }

static const char* ROOT[9] = {
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "6e340b9cffb37a989ca544e6bb780a2c78901d3fb33738768511a30617afa01d",
    "fac54203e7cc696cf0dfcb42c92a1d9dbaf70ad9e621f4bd8d98662f00e3c125", "aeb6bcfe274b70a14fb067a5e5578264db0fa9b51af5e0ba159158f329e06e77",
    "d37ee418976dd95753c1c73862b9398fa2a2cf9b4ff0fdfe8b30cd95209614b7", "4e3bbb1f7b478dcfe71fb631631519a3bca12c9aefca1612bfce4c13a86264d4",
    "76e67dadbcdf1e10e1b74ddc608abd2f98dfb16fbce75277b5232a127f2087ef", "ddb89be403809e325750d3d263cd78929c2942b7942a34b77e122c9594a74c8c",
    "5dc9da79a70659a9ad559cb701ded9a2ab9d823aad2f4960cfe370eff4604328",
};

int main(void) {
    static const uint8_t L0[] = {0}, L1[] = {0x10}, L2[] = {0x20, 0x21}, L3[] = {0x30, 0x31}, L4[] = {0x40, 0x41, 0x42, 0x43};
    static const uint8_t L5[] = {0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57}, L6[] = {0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f};
    const struct { const uint8_t* d; size_t n; } in[8] = { {L0, 0}, {L0, 1}, {L1, 1}, {L2, 2}, {L3, 2}, {L4, 4}, {L5, 8}, {L6, 16} };
    uint8_t leaves[8][CAS_HASH], root[CAS_HASH], want[CAS_HASH];
    for (int i = 0; i < 8; i++) merkle_leaf(in[i].d, in[i].n, leaves[i]);
    for (int n = 0; n <= 8; n++) { merkle_root((const uint8_t (*)[CAS_HASH])leaves, (size_t)n, root); unhex(ROOT[n], want); CHECK(!memcmp(root, want, CAS_HASH), "RFC 6962 root"); }

    /* пути включения: для каждого размера и листа путь проверяется, а подмена листа, номера, корня и пути - нет */
    for (int n = 1; n <= 8; n++) {
        merkle_root((const uint8_t (*)[CAS_HASH])leaves, (size_t)n, root);
        for (int i = 0; i < n; i++) {
            uint8_t path[8][CAS_HASH]; int pl = merkle_path((const uint8_t (*)[CAS_HASH])leaves, (size_t)n, (size_t)i, path, 8);
            CHECK(pl >= 0 && pl <= 3, "path length");
            CHECK(merkle_verify(leaves[i], (size_t)i, (size_t)n, (const uint8_t (*)[CAS_HASH])path, (size_t)pl, root) == 0, "verify");
            uint8_t bad[CAS_HASH]; memcpy(bad, leaves[i], CAS_HASH); bad[0] ^= 1;
            CHECK(merkle_verify(bad, (size_t)i, (size_t)n, (const uint8_t (*)[CAS_HASH])path, (size_t)pl, root) != 0, "forged leaf rejected");
            if (n > 1) CHECK(merkle_verify(leaves[i], (size_t)((i + 1) % n), (size_t)n, (const uint8_t (*)[CAS_HASH])path, (size_t)pl, root) != 0, "wrong index rejected");
            if (pl > 0) { path[0][5] ^= 0x80; CHECK(merkle_verify(leaves[i], (size_t)i, (size_t)n, (const uint8_t (*)[CAS_HASH])path, (size_t)pl, root) != 0, "forged path rejected"); }
            CHECK(merkle_verify(leaves[i], (size_t)i, (size_t)n, (const uint8_t (*)[CAS_HASH])path, (size_t)pl + 1, root) != 0, "extra path element rejected");
        }
    }
    CHECK(merkle_path((const uint8_t (*)[CAS_HASH])leaves, 8, 8, 0, 0) == CAS_ERR_ARG, "index out of range");
    { uint8_t path[1][CAS_HASH]; CHECK(merkle_path((const uint8_t (*)[CAS_HASH])leaves, 8, 3, path, 1) == CAS_ERR_SPACE, "path buffer too small"); }
    /* большое дерево: 1000 листьев, все пути проверяются */
    { static uint8_t big[1000][CAS_HASH]; for (int i = 0; i < 1000; i++) { uint8_t b[4] = {(uint8_t)i, (uint8_t)(i >> 8), 7, 7}; merkle_leaf(b, 4, big[i]); }
      merkle_root((const uint8_t (*)[CAS_HASH])big, 1000, root); int bad = 0;
      for (int i = 0; i < 1000; i += 7) { uint8_t path[64][CAS_HASH]; int pl = merkle_path((const uint8_t (*)[CAS_HASH])big, 1000, (size_t)i, path, 64);
          if (pl < 0 || merkle_verify(big[i], (size_t)i, 1000, (const uint8_t (*)[CAS_HASH])path, (size_t)pl, root)) bad++; }
      CHECK(bad == 0, "1000-leaf tree paths"); }

    /* хранилище */
    static cas_entry ent[16]; static uint8_t arena[4096]; cas_store st; cas_init(&st, ent, 16, arena, sizeof arena);
    uint8_t h1[CAS_HASH], h2[CAS_HASH], h3[CAS_HASH]; const uint8_t* d; size_t n;
    CHECK(cas_put(&st, "hello", 5, h1) == 0 && cas_put(&st, "world!", 6, h2) == 0, "put");
    CHECK(cas_put(&st, "hello", 5, h3) == 0 && !memcmp(h1, h3, CAS_HASH) && st.count == 2 && st.top == 11, "dedup stores once");
    CHECK(cas_get(&st, h1, &d, &n) == 0 && n == 5 && !memcmp(d, "hello", 5), "get");
    uint8_t ghost[CAS_HASH] = {1}; CHECK(cas_get(&st, ghost, &d, &n) == CAS_ERR_NOTFOUND && !cas_has(&st, ghost) && cas_has(&st, h1), "missing");
    arena[0] ^= 0xFF; CHECK(cas_get(&st, h1, &d, &n) == CAS_ERR_CORRUPT, "tampered data detected on read"); arena[0] ^= 0xFF;
    CHECK(cas_get(&st, h1, &d, &n) == 0, "restored");
    CHECK(cas_put(&st, "", 0, h3) == 0 && cas_get(&st, h3, &d, &n) == 0 && n == 0, "empty object");
    cas_release(&st, h1); CHECK(cas_gc(&st) == 0, "still referenced once");
    cas_release(&st, h1); CHECK(cas_release(&st, h1) == CAS_ERR_ARG, "over-release rejected");
    CHECK(cas_gc(&st) == 5 && !cas_has(&st, h1) && cas_get(&st, h2, &d, &n) == 0 && !memcmp(d, "world!", 6), "gc compacts and keeps neighbours");
    /* переполнение */
    { static cas_entry e2[2]; static uint8_t a2[8]; cas_store s2; cas_init(&s2, e2, 2, a2, sizeof a2); uint8_t h[CAS_HASH];
      CHECK(cas_put(&s2, "12345", 5, h) == 0 && cas_put(&s2, "123456", 6, h) == CAS_ERR_FULL, "arena full");
      CHECK(cas_put(&s2, "a", 1, h) == 0 && cas_put(&s2, "b", 1, h) == CAS_ERR_FULL, "entry table full while arena has room"); }

    /* файл кусками */
    { static uint8_t data[10000], back[10000], scratch[4096]; static cas_entry e3[64]; static uint8_t a3[20000]; cas_store s3; cas_init(&s3, e3, 64, a3, sizeof a3);
      for (int i = 0; i < 10000; i++) data[i] = (uint8_t)(i * 31 + (i >> 8));
      uint8_t mf[CAS_HASH], r1[CAS_HASH], r2[CAS_HASH]; size_t got;
      CHECK(cas_file_scratch(10000, 1000) == 16 + 10 * 64, "scratch size");
      CHECK(cas_put_file(&s3, data, 10000, 1000, scratch, sizeof scratch, mf, r1) == 0, "put_file");
      CHECK(cas_get_file(&s3, mf, back, sizeof back, &got) == 0 && got == 10000 && !memcmp(back, data, 10000), "get_file roundtrip");
      uint8_t lv[10][CAS_HASH]; for (int i = 0; i < 10; i++) { uint8_t c[CAS_HASH]; extern void sha256_hash_for_test(const void*, size_t, uint8_t*); sha256_hash_for_test(data + i * 1000, 1000, c); merkle_leaf(c, CAS_HASH, lv[i]); }
      merkle_root((const uint8_t (*)[CAS_HASH])lv, 10, r2); CHECK(!memcmp(r1, r2, CAS_HASH), "file root = Merkle root over chunk hashes");
      uint8_t mf2[CAS_HASH]; CHECK(cas_put_file(&s3, data, 10000, 1000, scratch, sizeof scratch, mf2, 0) == 0 && !memcmp(mf, mf2, CAS_HASH), "same file -> same manifest");
      CHECK(cas_put_file(&s3, data, 10000, 4096, scratch, 100, mf2, 0) == CAS_ERR_SPACE, "scratch too small");
      CHECK(cas_get_file(&s3, mf, back, 9999, &got) == CAS_ERR_SPACE, "output too small");
      int idx = -1; for (size_t i = 0; i < s3.max; i++) if (s3.e[i].used && s3.e[i].len == 1000) { idx = (int)i; break; }
      a3[s3.e[idx].off + 17] ^= 0x55; CHECK(cas_get_file(&s3, mf, back, sizeof back, &got) == CAS_ERR_CORRUPT, "damaged chunk detected");
      uint8_t em[CAS_HASH], er[CAS_HASH]; CHECK(cas_put_file(&s3, "", 0, 100, scratch, sizeof scratch, em, er) == 0 && cas_get_file(&s3, em, back, sizeof back, &got) == 0 && got == 0, "empty file"); }
    printf(fails ? "CAS FAILED\n" : "cas: RFC 6962 vectors, inclusion proofs, tamper detection, dedup, gc, chunked files passed\n");
    return fails != 0;
}
#include "../sha256.h"
void sha256_hash_for_test(const void* d, size_t n, uint8_t* out) { sha256_ctx_t c; sha256_init(&c); sha256_update(&c, d, n); sha256_final(&c, out); }
