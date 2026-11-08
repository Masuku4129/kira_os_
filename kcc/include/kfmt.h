/* kcc libc-lite: форматирование double для printf (%f %e %g), подключается из stdio.h только при `#define KCC_PRINTF_FLOAT`.
   Отдельный файл нужен потому, что препроцессор kcc токенизирует и пропущенные ветки #if, а арена компилятора внутри Киры - 4 МБ. */
#ifndef KCC_KFMT_H
#define KCC_KFMT_H
#ifdef __KCC_NOSSE__
#define KCC_FLT_CASE
#else
#include <string.h>
/* Точные десятичные цифры конечного неотрицательного double: значение = 0.d1d2...dn * 10^dp, цифры без ведущих и хвостовых нулей (нуль - "0", dp = 1).
   m * 2^e: при e >= 0 умножением на 2, при e < 0 как m * 5^k * 10^-k (k = -e). Большое число - основание 1e9, не больше 125 разрядов. */
static char kf_buf[1500];
static int kf_digits(double v, char *dg, int *dp) {
    unsigned long long bits; memcpy(&bits, &v, 8);
    int ex = (int)((bits >> 52) & 0x7ff);
    unsigned long long m = bits & 0xfffffffffffffULL;
    if (ex) m |= 1ULL << 52; else ex = 1;
    ex -= 1075;
    if (m == 0) { dg[0] = '0'; dg[1] = 0; *dp = 1; return 1; }
    unsigned limb[130]; int nl = 0;
    while (m) { limb[nl++] = (unsigned)(m % 1000000000ULL); m /= 1000000000ULL; }
    int k = ex < 0 ? -ex : 0, left = ex < 0 ? k : ex;
    while (left > 0) {
        int sh = left > (ex < 0 ? 13 : 29) ? (ex < 0 ? 13 : 29) : left;
        unsigned long long mul = 1;
        for (int i = 0; i < sh; i++) mul *= ex < 0 ? 5 : 2;
        unsigned long long carry = 0;
        for (int i = 0; i < nl; i++) { unsigned long long t = (unsigned long long)limb[i] * mul + carry; limb[i] = (unsigned)(t % 1000000000ULL); carry = t / 1000000000ULL; }
        while (carry) { limb[nl++] = (unsigned)(carry % 1000000000ULL); carry /= 1000000000ULL; }
        left -= sh;
    }
    int n = 0;
    for (int i = nl - 1; i >= 0; i--) {
        char t[9]; unsigned x = limb[i];
        for (int j = 8; j >= 0; j--) { t[j] = (char)('0' + x % 10); x /= 10; }
        for (int j = 0; j < 9; j++) if (n || t[j] != '0') dg[n++] = t[j];
    }
    *dp = n - k;
    while (n > 1 && dg[n - 1] == '0') n--;
    dg[n] = 0;
    return n;
}
/* Округление цифр до keep значащих (можно <= 0), к ближайшему, при точной середине - к чётному. Цифры дополняются ведущим нулём ради переноса. */
static int kf_round(char *dg, int nd, int *dp, int keep) {
    for (int i = nd; i >= 0; i--) dg[i + 1] = dg[i];
    dg[0] = '0';
    nd++; (*dp)++; keep++;
    if (keep <= 0) { dg[0] = '0'; nd = 1; keep = 1; }
    else if (keep < nd) {
        int up = 0;
        if (dg[keep] > '5') up = 1;
        else if (dg[keep] == '5') {
            for (int i = keep + 1; i < nd; i++) if (dg[i] != '0') up = 1;
            if (!up) up = keep > 0 && ((dg[keep - 1] - '0') & 1);
        }
        nd = keep;
        for (int i = keep - 1; up && i >= 0; i--) { if (dg[i] == '9') dg[i] = '0'; else { dg[i]++; up = 0; } }
    }
    if (dg[0] == '0') {
        int z = 1;
        while (z < nd && dg[z] == '0') z++;
        if (z == nd) { dg[0] = '0'; nd = 1; *dp = 1; return 1; }
        for (int i = z; i < nd; i++) dg[i - z] = dg[i];
        nd -= z; *dp -= z;
    }
    dg[nd] = 0;
    return nd;
}
/* Печать double в kf_buf (без знака); возвращает длину. conv: f F e E g G. */
static int kf_format(double v, char conv, int prec, int alt) {
    char *o = kf_buf; int n = 0;
    unsigned long long bits; memcpy(&bits, &v, 8);
    int up = conv == 'F' || conv == 'E' || conv == 'G';
    if (((bits >> 52) & 0x7ff) == 0x7ff) {
        const char *w = (bits & 0xfffffffffffffULL) ? (up ? "NAN" : "nan") : (up ? "INF" : "inf");
        for (; *w; w++) o[n++] = *w;
        return n;
    }
    if (prec < 0) prec = 6;
    if (prec > 1000) prec = 1000;
    char dg[1200]; int dp;
    int nd = kf_digits(v < 0 ? -v : v, dg, &dp);
    int isz = dg[0] == '0';
    char c = up ? (char)(conv + 32) : conv;
    if (c == 'g') {
        int P = prec == 0 ? 1 : prec;
        char t[1200]; int tdp = dp, tn = nd;
        for (int i = 0; i <= nd; i++) t[i] = dg[i];
        tn = kf_round(t, tn, &tdp, P);
        int X = isz ? 0 : tdp - 1;
        if (X < -4 || X >= P) { c = 'e'; prec = P - 1; } else { c = 'f'; prec = P - 1 - X; }
    }
    int strip = (conv == 'g' || conv == 'G') && !alt;
    if (c == 'f') {
        nd = kf_round(dg, nd, &dp, dp + prec);
        if (dp > 0) for (int i = 0; i < dp; i++) o[n++] = i < nd ? dg[i] : '0';
        else o[n++] = '0';
        int fstart = n;
        if (prec > 0 || alt) o[n++] = '.';
        for (int j = 0; j < prec; j++) { int idx = dp + j; o[n++] = idx >= 0 && idx < nd ? dg[idx] : '0'; }
        if (strip && prec > 0) { while (n > fstart && o[n - 1] == '0') n--; if (o[n - 1] == '.') n--; }
    } else {
        nd = kf_round(dg, nd, &dp, prec + 1);
        int X = isz ? 0 : dp - 1;
        o[n++] = dg[0];
        int fstart = n;
        if (prec > 0 || alt) o[n++] = '.';
        for (int j = 1; j <= prec; j++) o[n++] = j < nd ? dg[j] : '0';
        if (strip && prec > 0) { while (n > fstart && o[n - 1] == '0') n--; if (o[n - 1] == '.') n--; }
        o[n++] = up ? 'E' : 'e';
        if (X < 0) { o[n++] = '-'; X = -X; } else o[n++] = '+';
        if (X >= 100) o[n++] = (char)('0' + X / 100);
        o[n++] = (char)('0' + X / 10 % 10);
        o[n++] = (char)('0' + X % 10);
    }
    return n;
}
/* ветка vsnprintf (stdio.h): в цепочке else-if после целочисленных преобразований */
#define KCC_FLT_CASE \
    else if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') { \
        double dv = va_arg(ap, double); \
        unsigned long long db; memcpy(&db, &dv, 8); \
        sg = (db >> 63) ? '-' : plus ? '+' : spc ? ' ' : 0; \
        len = kf_format(dv, c, prec, alt); \
        str = kf_buf; \
        if (((db >> 52) & 0x7ff) == 0x7ff) zero = 0; \
    }
#endif
#endif
