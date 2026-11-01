/* Просмотр файлов (программа Ring 3 / EL0): Проводник открывает файл двойным щелчком, ядро выдаёт программе право
   читать только его. Разбор чужих файлов идёт здесь, а не в ядре: ошибка в разборе не трогает систему.
   Форматы: PNG (свой inflate: блоки stored/fixed/dynamic, фильтры, цвет 0/2/3/4/6, глубина 1-16, без чересстрочности),
   BMP (1/4/8/24/32 бита без сжатия), PPM/PGM (P6/P5, 8 бит), GIF (87a/89a, первый кадр: LZW, общая и своя палитры,
   чересстрочность, прозрачный цвет - как фон), текст UTF-8, остальное - шестнадцатерично.
   Клавиши: стрелки, PgUp/PgDn, Home/End - прокрутка текста; q или Esc - закрыть. */
#include "kira.h"
#include "../font.h"
KIRA_ENTRY()

#define W 720
#define H 540
#define TOP 20
#define MAXFILE (1u << 20)                    /* 1 МБ файла */
#define MAXPIX  (1u << 20)                    /* 1 Мпикс картинки: 4 МБ кучи на программу */

static u32* px;
static u8* file; static u32 flen;
static u32* img; static u32 iw, ih;           /* картинка 0x00RRGGBB */
static char path[128], info[96], err[64];
static int mode;                              /* 0 - картинка, 1 - текст, 2 - шестнадцатерично */
static u32 scroll;

/* ---------- память: куски кучи подряд ---------- */
static u8* heap_end; static u64 heap_left;
static void* alloc(u64 n) {
    n = (n + 15) & ~15ull;
    if (n > heap_left) {
        u64 pages = (n - heap_left + 4095) / 4096;
        u8* p = (u8*)k_heap_grow(pages);
        if (!p) return 0;
        if (!heap_end) heap_end = p;
        heap_left += pages * 4096;
    }
    void* r = heap_end; heap_end += n; heap_left -= n;
    return r;
}

static u32 slen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static void scat(char* d, const char* s, u32 cap) { u32 n = slen(d), i = 0; while (s[i] && n + 1 < cap) d[n++] = s[i++]; d[n] = 0; }
static void sdec(char* d, u64 v, u32 cap) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); scat(d, b + i, cap); }
static void fail(const char* m) { err[0] = 0; scat(err, m, sizeof err); }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u32 le32(const u8* p) { return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0]; }
static u32 le16(const u8* p) { return (u32)p[1] << 8 | p[0]; }

/* ---------- inflate (RFC 1951) ---------- */
typedef struct { const u8* s; u32 n, pos; u32 bit, bits; } bits_t;
static int getbit(bits_t* b) {
    if (!b->bits) { if (b->pos >= b->n) return -1; b->bit = b->s[b->pos++]; b->bits = 8; }
    int v = (int)(b->bit & 1); b->bit >>= 1; b->bits--; return v;
}
static int getbits(bits_t* b, int n) { int v = 0; for (int i = 0; i < n; i++) { int x = getbit(b); if (x < 0) return -1; v |= x << i; } return v; }

typedef struct { u16 count[16], sym[320]; } huff_t;
static int build(huff_t* h, const u8* len, int n) {
    u16 offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (u16)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (u16)i;
    return 0;
}
static int decode(bits_t* b, const huff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int x = getbit(b); if (x < 0) return -1;
        code |= x;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static const u16 lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int codes(bits_t* b, u8* out, u32 cap, u32* o, const huff_t* lit, const huff_t* dist) {
    for (;;) {
        int s = decode(b, lit);
        if (s < 0) return -1;
        if (s < 256) { if (*o >= cap) return -1; out[(*o)++] = (u8)s; continue; }
        if (s == 256) return 0;
        s -= 257; if (s >= 29) return -1;
        int e = getbits(b, lext[s]); if (e < 0) return -1;
        u32 len = lbase[s] + (u32)e;
        int ds = decode(b, dist); if (ds < 0 || ds >= 30) return -1;
        int de = getbits(b, dext[ds]); if (de < 0) return -1;
        u32 d = dbase[ds] + (u32)de;
        if (d > *o || *o + len > cap) return -1;
        for (u32 i = 0; i < len; i++) { out[*o] = out[*o - d]; (*o)++; }
    }
}

static int inflate(const u8* src, u32 n, u8* out, u32 cap, u32* outlen) {
    bits_t b = { src, n, 0, 0, 0 };
    static huff_t lit, dist;
    u32 o = 0; int last;
    do {
        last = getbit(&b);
        int type = getbits(&b, 2);
        if (last < 0 || type < 0) return -1;
        if (type == 0) {                                         /* без сжатия */
            b.bits = 0;
            if (b.pos + 4 > n) return -1;
            u32 len = le16(src + b.pos), nlen = le16(src + b.pos + 2); b.pos += 4;
            if ((len ^ 0xFFFF) != nlen || b.pos + len > n || o + len > cap) return -1;
            for (u32 i = 0; i < len; i++) out[o++] = src[b.pos++];
        } else if (type == 1) {                                  /* фиксированные коды */
            u8 l[320];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            build(&lit, l, 288);
            for (int i = 0; i < 30; i++) l[i] = 5;
            build(&dist, l, 30);
            if (codes(&b, out, cap, &o, &lit, &dist)) return -1;
        } else if (type == 2) {                                  /* коды в блоке */
            int nlen = getbits(&b, 5) + 257, ndist = getbits(&b, 5) + 1, ncode = getbits(&b, 4) + 4;
            if (nlen > 286 || ndist > 30 || ncode < 4) return -1;
            static const u8 ord[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            u8 l[320];
            for (int i = 0; i < 19; i++) l[i] = 0;
            for (int i = 0; i < ncode; i++) { int v = getbits(&b, 3); if (v < 0) return -1; l[ord[i]] = (u8)v; }
            huff_t cl;
            if (build(&cl, l, 19)) return -1;
            int i = 0;
            while (i < nlen + ndist) {
                int s = decode(&b, &cl);
                if (s < 0) return -1;
                if (s < 16) { l[i++] = (u8)s; continue; }
                int rep, val = 0;
                if (s == 16) { if (!i) return -1; val = l[i - 1]; rep = 3 + getbits(&b, 2); }
                else if (s == 17) rep = 3 + getbits(&b, 3);
                else rep = 11 + getbits(&b, 7);
                if (rep < 3 || i + rep > nlen + ndist) return -1;
                while (rep--) l[i++] = (u8)val;
            }
            if (build(&lit, l, nlen) || build(&dist, l + nlen, ndist)) return -1;
            if (codes(&b, out, cap, &o, &lit, &dist)) return -1;
        } else return -1;
    } while (!last);
    *outlen = o;
    return 0;
}

/* ---------- PNG ---------- */
static int paeth(int a, int b, int c) { int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p; return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }

static int load_png(void) {
    if (flen < 33 || be32(file + 12) != 0x49484452) { fail("PNG: нет заголовка IHDR"); return -1; }
    u32 w = be32(file + 16), h = be32(file + 20);
    u32 depth = file[24], ct = file[25], inter = file[28];
    if (!w || !h || (u64)w * h > MAXPIX) { fail("PNG: размер больше 1 Мпикс"); return -1; }
    if (inter) { fail("PNG: чересстрочный - пока нет"); return -1; }
    u32 ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!ch || !(depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) || (depth < 8 && ct != 0 && ct != 3) || (depth == 16 && ct == 3)) { fail("PNG: неподдерживаемый цвет"); return -1; }
    u32 pal[256]; u8 alpha[256];
    for (int i = 0; i < 256; i++) { pal[i] = 0; alpha[i] = 255; }
    u8* z = (u8*)alloc(flen); u32 zn = 0;
    if (!z) { fail("нет памяти"); return -1; }
    for (u32 p = 8; p + 12 <= flen; ) {                        /* куски: IDAT подряд в один поток */
        u32 len = be32(file + p), type = be32(file + p + 4);
        if (len > flen - p - 12) { fail("PNG: кусок за концом файла"); return -1; }
        const u8* d = file + p + 8;
        if (type == 0x504C5445) for (u32 i = 0; i < len / 3 && i < 256; i++) pal[i] = (u32)d[3 * i] << 16 | (u32)d[3 * i + 1] << 8 | d[3 * i + 2];
        else if (type == 0x74524E53 && ct == 3) for (u32 i = 0; i < len && i < 256; i++) alpha[i] = d[i];
        else if (type == 0x49444154) { for (u32 i = 0; i < len; i++) z[zn++] = d[i]; }
        else if (type == 0x49454E44) break;
        p += len + 12;
    }
    if (zn < 2 || (z[0] & 15) != 8) { fail("PNG: нет данных zlib"); return -1; }
    u32 bpp_bits = ch * depth, row = (w * bpp_bits + 7) / 8, bpp = (bpp_bits + 7) / 8;
    u32 cap = h * (row + 1), got = 0;
    u8* raw = (u8*)alloc(cap);
    img = (u32*)alloc((u64)w * h * 4);
    if (!raw || !img) { fail("нет памяти под картинку"); return -1; }
    if (inflate(z + 2, zn - 2, raw, cap, &got) || got < cap) { fail("PNG: повреждённые данные"); return -1; }
    for (u32 y = 0; y < h; y++) {                             /* фильтры строк */
        u8* r = raw + y * (row + 1); u8 f = r[0]; u8* c = r + 1;
        u8* up = y ? raw + (y - 1) * (row + 1) + 1 : 0;
        for (u32 x = 0; x < row; x++) {
            int a = x >= bpp ? c[x - bpp] : 0, b = up ? up[x] : 0, cc = (up && x >= bpp) ? up[x - bpp] : 0;
            if (f == 1) c[x] = (u8)(c[x] + a);
            else if (f == 2) c[x] = (u8)(c[x] + b);
            else if (f == 3) c[x] = (u8)(c[x] + ((a + b) >> 1));
            else if (f == 4) c[x] = (u8)(c[x] + paeth(a, b, cc));
            else if (f != 0) { fail("PNG: неверный фильтр"); return -1; }
        }
    }
    for (u32 y = 0; y < h; y++) {
        const u8* c = raw + y * (row + 1) + 1;
        for (u32 x = 0; x < w; x++) {
            u32 r, g, b, a = 255;
            if (depth < 8) {                                     /* 1/2/4 бита: серый или палитра */
                u32 bit = x * depth, v = (c[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
                if (ct == 3) { r = pal[v] >> 16; g = (pal[v] >> 8) & 255; b = pal[v] & 255; a = alpha[v]; }
                else r = g = b = v * 255 / ((1u << depth) - 1);
            } else {
                u32 s = depth / 8; const u8* q = c + x * ch * s;  /* 16 бит - старший байт */
                if (ct == 0) r = g = b = q[0];
                else if (ct == 2) { r = q[0]; g = q[s]; b = q[2 * s]; }
                else if (ct == 3) { r = pal[q[0]] >> 16; g = (pal[q[0]] >> 8) & 255; b = pal[q[0]] & 255; a = alpha[q[0]]; }
                else if (ct == 4) { r = g = b = q[0]; a = q[s]; }
                else { r = q[0]; g = q[s]; b = q[2 * s]; a = q[3 * s]; }
            }
            u32 bg = ((x / 8 + y / 8) & 1) ? 0xC0 : 0x90;        /* прозрачность - на клетках */
            r = (r * a + bg * (255 - a)) / 255; g = (g * a + bg * (255 - a)) / 255; b = (b * a + bg * (255 - a)) / 255;
            img[y * w + x] = r << 16 | g << 8 | b;
        }
    }
    iw = w; ih = h;
    info[0] = 0; scat(info, "PNG ", sizeof info); sdec(info, w, sizeof info); scat(info, "x", sizeof info); sdec(info, h, sizeof info);
    scat(info, ", цвет ", sizeof info); sdec(info, ct, sizeof info); scat(info, ", ", sizeof info); sdec(info, depth, sizeof info); scat(info, " бит", sizeof info);
    return 0;
}

/* ---------- BMP ---------- */
static int load_bmp(void) {
    if (flen < 54) { fail("BMP: короткий файл"); return -1; }
    u32 off = le32(file + 10), hs = le32(file + 14);
    int w = (int)le32(file + 18), h = (int)le32(file + 22);
    u32 bpp = le16(file + 28), comp = le32(file + 30);
    int flip = h > 0; if (h < 0) h = -h;
    if (w <= 0 || !h || (u64)w * (u64)h > MAXPIX) { fail("BMP: размер больше 1 Мпикс"); return -1; }
    if (!(comp == 0 || (comp == 3 && bpp == 32)) || !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32)) { fail("BMP: сжатие или глубина не поддерживаются"); return -1; }
    u32 stride = ((u32)w * bpp + 31) / 32 * 4;
    if (off > flen || (u64)stride * (u64)h > flen - off) { fail("BMP: данные за концом файла"); return -1; }
    const u8* pal = file + 14 + hs;
    img = (u32*)alloc((u64)w * h * 4);
    if (!img) { fail("нет памяти под картинку"); return -1; }
    for (int y = 0; y < h; y++) {
        const u8* r = file + off + (u64)stride * (u32)(flip ? h - 1 - y : y);
        for (int x = 0; x < w; x++) {
            u32 c;
            if (bpp >= 24) { const u8* q = r + x * (int)(bpp / 8); c = (u32)q[2] << 16 | (u32)q[1] << 8 | q[0]; }
            else {
                u32 bit = (u32)x * bpp, v = (r[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1);
                const u8* q = pal + v * 4;
                c = (q + 3 < file + flen) ? (u32)q[2] << 16 | (u32)q[1] << 8 | q[0] : 0;
            }
            img[y * w + x] = c;
        }
    }
    iw = (u32)w; ih = (u32)h;
    info[0] = 0; scat(info, "BMP ", sizeof info); sdec(info, iw, sizeof info); scat(info, "x", sizeof info); sdec(info, ih, sizeof info);
    scat(info, ", ", sizeof info); sdec(info, bpp, sizeof info); scat(info, " бит", sizeof info);
    return 0;
}

/* ---------- PPM / PGM (P6, P5) ---------- */
static u32 pnm_num(u32* p) {
    while (*p < flen) {
        u8 c = file[*p];
        if (c == '#') { while (*p < flen && file[*p] != '\n') (*p)++; }
        else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') (*p)++;
        else break;
    }
    u32 v = 0; while (*p < flen && file[*p] >= '0' && file[*p] <= '9') v = v * 10 + (u32)(file[(*p)++] - '0');
    return v;
}
static int load_pnm(void) {
    int color = file[1] == '6'; u32 p = 2;
    u32 w = pnm_num(&p), h = pnm_num(&p), mx = pnm_num(&p); p++;
    u32 ch = color ? 3 : 1;
    if (!w || !h || (u64)w * h > MAXPIX || !mx || mx > 255) { fail("PPM: размер или 16 бит не поддерживаются"); return -1; }
    if ((u64)w * h * ch > flen - p) { fail("PPM: данные за концом файла"); return -1; }
    img = (u32*)alloc((u64)w * h * 4);
    if (!img) { fail("нет памяти под картинку"); return -1; }
    for (u32 i = 0; i < w * h; i++) {
        const u8* q = file + p + i * ch;
        u32 r = q[0] * 255 / mx, g = q[ch > 1] * 255 / mx, b = q[ch > 1 ? 2 : 0] * 255 / mx;
        img[i] = r << 16 | g << 8 | b;
    }
    iw = w; ih = h;
    info[0] = 0; scat(info, color ? "PPM " : "PGM ", sizeof info); sdec(info, w, sizeof info); scat(info, "x", sizeof info); sdec(info, h, sizeof info);
    return 0;
}

/* ---------- GIF: первый кадр ---------- */
static int load_gif(void) {
    if (flen < 13) { fail("GIF: короткий файл"); return -1; }
    u32 sw = le16(file + 6), sh = le16(file + 8), fl = file[10], p = 13;
    u32 gpal = 0, gn = 0, bg = file[11];
    if (fl & 0x80) { gn = 2u << (fl & 7); gpal = p; p += 3 * gn; if (p > flen) { fail("GIF: палитра за концом файла"); return -1; } }
    int trans = -1;
    while (p < flen) {
        u8 b = file[p++];
        if (b == 0x3B) break;                                    /* конец файла без кадра */
        if (b == 0x21) {                                         /* расширение: управление кадром - прозрачный цвет */
            if (p + 1 > flen) break;
            u8 label = file[p++];
            if (label == 0xF9 && p + 5 <= flen && file[p] >= 4 && (file[p + 1] & 1)) trans = file[p + 4];
            while (p < flen && file[p]) { p += 1u + file[p]; }
            p++;
            continue;
        }
        if (b != 0x2C) { fail("GIF: неизвестный блок"); return -1; }
        if (p + 9 > flen) { fail("GIF: короткий заголовок кадра"); return -1; }
        u32 w = le16(file + p + 4), h = le16(file + p + 6), f2 = file[p + 8]; p += 9;
        u32 pal = gpal, pn = gn;
        if (f2 & 0x80) { pn = 2u << (f2 & 7); pal = p; p += 3 * pn; if (p > flen) { fail("GIF: палитра кадра за концом"); return -1; } }
        if (!pn) { fail("GIF: нет палитры"); return -1; }
        if (!w || !h || (u64)w * h > MAXPIX) { fail("GIF: размер больше 1 Мпикс"); return -1; }
        if (p >= flen) { fail("GIF: нет данных кадра"); return -1; }
        u32 mcs = file[p++];
        if (mcs < 2 || mcs > 8) { fail("GIF: неверный размер кода LZW"); return -1; }
        u32 npx = w * h;
        u8* idx = (u8*)alloc(npx);
        static u16 prefix[4096]; static u8 suffix[4096], stack[4097];
        img = (u32*)alloc((u64)npx * 4);
        if (!idx || !img) { fail("нет памяти под картинку"); return -1; }
        for (u32 i = 0; i < npx; i++) idx[i] = (u8)(trans >= 0 ? (u32)trans : bg);
        /* LZW по подблокам: коды младшими битами вперёд, размер растёт до 12 бит, код очистки - сброс таблицы */
        u32 clear = 1u << mcs, eoi = clear + 1, next = clear + 2, size = mcs + 1, out = 0;
        u32 bits = 0, nb = 0, blk = 0; int prev = -1; u8 first = 0;
        for (u32 i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (u8)i; }
        for (;;) {
            while (nb < size) {
                if (!blk) { if (p >= flen) goto done; blk = file[p++]; if (!blk) goto done; }
                if (p >= flen) goto done;
                bits |= (u32)file[p++] << nb; nb += 8; blk--;
            }
            u32 code = bits & ((1u << size) - 1); bits >>= size; nb -= size;
            if (code == clear) { next = clear + 2; size = mcs + 1; prev = -1; continue; }
            if (code == eoi) break;
            u32 c = code, sp = 0;
            if (prev < 0) {
                if (code >= clear) { fail("GIF: испорченные данные LZW"); return -1; }
                if (out < npx) idx[out++] = (u8)code;
                prev = (int)code; first = (u8)code; continue;
            }
            if (code > next || (code == next && next >= 4096)) { fail("GIF: испорченные данные LZW"); return -1; }
            if (code == next) { stack[sp++] = first; c = (u32)prev; }     /* KwKwK: код ещё не в таблице */
            while (c >= clear) { if (sp >= 4096 || c >= 4096) { fail("GIF: испорченные данные LZW"); return -1; } stack[sp++] = suffix[c]; c = prefix[c]; }
            stack[sp++] = (u8)c; first = (u8)c;
            while (sp && out < npx) idx[out++] = stack[--sp];
            if (next < 4096) { prefix[next] = (u16)prev; suffix[next] = first; next++; if (next == (1u << size) && size < 12) size++; }
            prev = (int)code;
        }
    done:
        for (u32 y = 0, src = 0, pass = 0, step = 8, row = 0; src < h; src++) {   /* чересстрочно: строки 0,8.. 4,12.. 2,6.. 1,3.. */
            if (f2 & 0x40) {
                y = row;
                row += step;
                while (row >= h && pass < 3) { pass++; step = pass == 1 ? 8 : pass == 2 ? 4 : 2; row = pass == 1 ? 4 : pass == 2 ? 2 : 1; }
            } else y = src;
            for (u32 x = 0; x < w; x++) {
                u32 v = idx[src * w + x];
                const u8* q = file + pal + 3 * (v < pn ? v : 0);
                img[y * w + x] = (int)v == trans ? 0 : (u32)q[0] << 16 | (u32)q[1] << 8 | q[2];
            }
        }
        iw = w; ih = h;
        info[0] = 0; scat(info, "GIF ", sizeof info); sdec(info, w, sizeof info); scat(info, "x", sizeof info); sdec(info, h, sizeof info);
        scat(info, ", палитра ", sizeof info); sdec(info, pn, sizeof info);
        if (f2 & 0x40) scat(info, ", чересстрочный", sizeof info);
        if (out < npx) scat(info, ", данные кадра неполные", sizeof info);
        (void)sw; (void)sh;
        return 0;
    }
    fail("GIF: в файле нет кадра"); return -1;
}

/* ---------- отрисовка ---------- */
static u32 utf8(const u8* s, u32 n, u32* cp) {
    u8 c = s[0];
    if (c < 0x80 || n < 2) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { *cp = (u32)(c & 0x1F) << 6 | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && n >= 3) { *cp = (u32)(c & 0x0F) << 12 | (u32)(s[1] & 0x3F) << 6 | (s[2] & 0x3F); return 3; }
    *cp = '?'; return 1;
}
static void glyph(int x, int y, u32 cp, u32 fg, u32 bg) {
    const u8* g = font_glyph(cp);
    for (int r = 0; r < FONT_H; r++) { u32* row = px + (y + r) * W + x; for (int c = 0; c < FONT_W; c++) row[c] = (g[r] & (0x80 >> c)) ? fg : bg; }
}
static void text_at(int x, int y, const char* s, u32 fg, u32 bg) {
    u32 n = slen(s);
    for (u32 i = 0; i < n && x + FONT_W <= W; ) { u32 cp; i += utf8((const u8*)s + i, n - i, &cp); glyph(x, y, cp, fg, bg); x += FONT_W; }
}

static int is_text(void) {
    u32 bad = 0, n = flen < 4096 ? flen : 4096;
    for (u32 i = 0; i < n; ) {
        u8 c = file[i];
        if (c == 0) return 0;
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') bad++;
        u32 cp, k = utf8(file + i, n - i, &cp); i += k;
    }
    return bad * 20 < n + 1;
}

static void draw(void) {
    for (int i = 0; i < W * TOP; i++) px[i] = 0x00203040;
    char st[200]; st[0] = 0; scat(st, path, sizeof st); scat(st, "   ", sizeof st); scat(st, err[0] ? err : info, sizeof st);
    text_at(4, 2, st, 0x00E0E8F0, 0x00203040);
    for (int i = W * TOP; i < W * H; i++) px[i] = 0x00101820;
    int rows = (H - TOP) / FONT_H;
    if (err[0]) { text_at(16, TOP + 16, err, 0x00F08080, 0x00101820); }
    else if (mode == 0) {                                        /* картинка: целиком в окне, без увеличения */
        u32 aw = W, ah = H - TOP, sw = iw, sh = ih;
        if (sw > aw || sh > ah) { if ((u64)iw * ah > (u64)ih * aw) { sw = aw; sh = (u32)((u64)ih * aw / iw); } else { sh = ah; sw = (u32)((u64)iw * ah / ih); } }
        if (!sw) sw = 1;
        if (!sh) sh = 1;
        u32 ox = (aw - sw) / 2, oy = TOP + (ah - sh) / 2;
        for (u32 y = 0; y < sh; y++) {
            u32 sy = (u32)((u64)y * ih / sh);
            for (u32 x = 0; x < sw; x++) px[(oy + y) * W + ox + x] = img[sy * iw + (u32)((u64)x * iw / sw)];
        }
    } else if (mode == 1) {
        u32 line = 0, p = 0;
        while (p < flen && line < scroll) { if (file[p++] == '\n') line++; }
        for (int r = 0; r < rows && p < flen; r++) {
            int x = 0;
            while (p < flen && file[p] != '\n') {
                u32 cp, k = utf8(file + p, flen - p, &cp); p += k;
                if (cp == '\r') continue;
                if (cp == '\t') cp = ' ';
                if (x + FONT_W <= W) glyph(x, TOP + r * FONT_H, cp < 32 ? '?' : cp, 0x00D0D8E0, 0x00101820);
                x += FONT_W;
            }
            p++;
        }
    } else {
        static const char hx[] = "0123456789abcdef";
        for (int r = 0; r < rows; r++) {
            u32 o = (scroll + (u32)r) * 16;
            if (o >= flen) break;
            char ln[90]; int k = 0;
            for (int s = 28; s >= 0; s -= 4) ln[k++] = hx[(o >> s) & 15];
            ln[k++] = ' '; ln[k++] = ' ';
            for (u32 i = 0; i < 16; i++) { if (o + i < flen) { ln[k++] = hx[file[o + i] >> 4]; ln[k++] = hx[file[o + i] & 15]; } else { ln[k++] = ' '; ln[k++] = ' '; } ln[k++] = ' '; }
            ln[k++] = ' ';
            for (u32 i = 0; i < 16 && o + i < flen; i++) { u8 c = file[o + i]; ln[k++] = (char)(c >= 32 && c < 127 ? c : '.'); }
            ln[k] = 0;
            text_at(4, TOP + r * FONT_H, ln, 0x00B0C8E0, 0x00101820);
        }
    }
    k_gfx_present();
}

int umain(void) {
    if (k_grant_name(path, sizeof path) <= 0) { k_write("view: open a file from Files (double click)\n", 44); return 1; }
    i64 sz = k_grant_size();
    if (sz < 0) { k_write("view: file is not readable\n", 27); return 1; }
    px = k_gfx_create(W, H);
    if (!px) { k_write("view: cannot create window\n", 27); return 1; }
    flen = sz > MAXFILE ? MAXFILE : (u32)sz;
    file = (u8*)alloc(flen + 1);
    if (!file) fail("нет памяти под файл");
    else {
        u32 got = 0;
        while (got < flen) { i64 r = k_grant_read(file + got, flen - got > 65536 ? 65536 : flen - got, got); if (r <= 0) break; got += (u32)r; }
        flen = got;
    }
    if (file) {
        if (flen >= 8 && file[0] == 0x89 && file[1] == 'P' && file[2] == 'N' && file[3] == 'G') { mode = 0; load_png(); }
        else if (flen >= 2 && file[0] == 'B' && file[1] == 'M') { mode = 0; load_bmp(); }
        else if (flen >= 3 && file[0] == 'P' && (file[1] == '6' || file[1] == '5')) { mode = 0; load_pnm(); }
        else if (flen >= 6 && file[0] == 'G' && file[1] == 'I' && file[2] == 'F' && file[3] == '8') { mode = 0; load_gif(); }
        else if (is_text()) { mode = 1; info[0] = 0; scat(info, "текст, ", sizeof info); sdec(info, flen, sizeof info); scat(info, " байт", sizeof info); }
        else { mode = 2; info[0] = 0; scat(info, "двоичный, ", sizeof info); sdec(info, flen, sizeof info); scat(info, " байт", sizeof info); }
        if (sz > MAXFILE) scat(info, " (показан первый 1 МБ)", sizeof info);
    }
    char m[256]; m[0] = 0; scat(m, "view: ", sizeof m); scat(m, path, sizeof m); scat(m, " - ", sizeof m); scat(m, err[0] ? err : info, sizeof m);
    if (!err[0] && mode == 0) {                                   /* сумма пикселей: стенд сверяет разбор с эталоном */
        u32 sum = 0; for (u32 i = 0; i < iw * ih; i++) sum += img[i];
        static const char hx[] = "0123456789abcdef";
        char hs[16] = " sum ";
        for (int i = 0; i < 8; i++) hs[5 + i] = hx[(sum >> (28 - 4 * i)) & 15];
        hs[13] = 0;
        scat(m, hs, sizeof m);
    }
    scat(m, "\n", sizeof m);
    k_write(m, slen(m));
    draw();
    for (;;) {
        kevent_t ev; int dirty = 0;
        while (k_gfx_event(&ev) > 0) {
            if (ev.type == KEV_CLOSE) return 0;
            if (ev.type != KEV_KEY_DOWN) continue;
            u32 k = ev.a, rows = (H - TOP) / FONT_H;
            if (k == 'q' || k == 0x100) return 0;                    /* q, Esc */
            if (k == 0x118 && mode) { scroll++; dirty = 1; }         /* вниз */
            if (k == 0x117 && mode && scroll) { scroll--; dirty = 1; }
            if (k == 0x11E && mode) { scroll += rows; dirty = 1; }   /* PgDn */
            if (k == 0x11D && mode) { scroll = scroll > rows ? scroll - rows : 0; dirty = 1; }
            if (k == 0x11B) { scroll = 0; dirty = 1; }               /* Home */
        }
        if (dirty) draw();
        k_sleep(2);
    }
}
