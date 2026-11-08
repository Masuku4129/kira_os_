/* Широкие и Unicode-литералы, универсальные имена символов (wchar_t = int, u"" - 16 бит, U"" - 32 бита). */
#include <stddef.h>
static const wchar_t gw[] = L"abé\U0001F600z";
static const unsigned short g16[] = u"aé\U0001F600";
static const unsigned g32[] = U"a\U0001F600";
static const wchar_t *gp = L"ptr";
static wchar_t gbuf[8] = L"xy";
struct S { wchar_t w[4]; const unsigned short *u; };
static struct S gs = { L"hi", u"yo" };

static long wlen(const wchar_t *s) { long n = 0; while (s[n]) n++; return n; }

int main(void) {
    long t = 0;
    t = t * 31 + sizeof gw / sizeof gw[0];            /* ab, e-acute, эмодзи, z + NUL = 5 */
    for (size_t i = 0; i < sizeof gw / sizeof gw[0]; i++) t = t * 31 + gw[i];
    t = t * 31 + sizeof g16 / sizeof g16[0];          /* a, e-acute, суррогатная пара + NUL = 5 */
    for (size_t i = 0; i < sizeof g16 / sizeof g16[0]; i++) t = t * 31 + g16[i];
    t = t * 31 + sizeof g32 / sizeof g32[0];
    for (size_t i = 0; i < sizeof g32 / sizeof g32[0]; i++) t = t * 31 + g32[i];
    t = t * 31 + wlen(gp) + gp[1];
    t = t * 31 + gbuf[0] + gbuf[1] + gbuf[2] + sizeof gbuf;
    t = t * 31 + gs.w[0] + gs.w[1] + gs.w[2] + gs.u[0] + gs.u[1] + gs.u[2];
    wchar_t loc[] = L"local\x41\101";                 /* \x41 и \101 - 'A' */
    t = t * 31 + sizeof loc / sizeof loc[0] + loc[5] + loc[6];
    int ai[3] = { 1, 2, 3 };
    wchar_t part[6] = L"ab";
    t = t * 31 + part[0] + part[1] + part[2] + part[5] + ai[2];
    /* символьные константы */
    t = t * 31 + L'x' + L'é' + L'\x1234' + L'\0';
    t = t * 31 + u'é' + U'\U0001F600' + u'a' + U'\x10ffff';
    t = t * 31 + (L'\xffffffff' < 0) + (U'\xffffffff' > 0) + (u'\xffff' > 0);
    t = t * 31 + sizeof L'a' + sizeof u'a' + sizeof U'a' + sizeof L"abc" + sizeof u"abc" + sizeof U"abc";
    /* узкие строки с UCN: UTF-8 */
    const char *n = "é€\U0001F600";
    for (int i = 0; n[i]; i++) t = t * 31 + (unsigned char)n[i];
    t = t * 31 + sizeof "é";
    /* конкатенация узкой и широкой */
    const wchar_t *cc = "ab" L"cd" "é";
    t = t * 31 + wlen(cc) + cc[1] + cc[2] + cc[4];
    /* UTF-8 в исходном тексте широкого литерала и символьной константе */
    const wchar_t *src = L"é€";
    t = t * 31 + wlen(src) + src[0] + src[1] + L'é';
    return (int)(t & 0xffffff);
}
