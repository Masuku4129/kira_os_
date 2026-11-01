/* IPv6: адреса, контрольные суммы, заголовки расширения, сообщения Neighbor Discovery. Чистый код (см. ip6.h). */
#include "ip6.h"

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void cpy(void* d, const void* s, uint32_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static void zero(void* d, uint32_t n) { uint8_t* a = (uint8_t*)d; while (n--) *a++ = 0; }

/* ---------- адреса ---------- */
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int at_end(char c) { return c == 0 || c == ' '; }

int ip6_parse(const char* s, uint8_t out[16]) {
    uint16_t w[8]; int n = 0, gap = -1;
    if (s[0] == ':') {                                                /* «::» в начале */
        if (s[1] != ':') return -1;
        gap = 0; s += 2;
    }
    while (!(gap == 0 && n == 0 && at_end(*s))) {                     /* «::» целиком - групп нет */
        const char* g = s; uint32_t v = 0; int d = 0;
        while (hexv(*s) >= 0) { v = v * 16 + (uint32_t)hexv(*s); if (++d > 4) return -1; s++; }
        if (d == 0) return -1;
        if (*s == '.') {                                              /* хвост в виде IPv4: a.b.c.d */
            uint32_t q[4]; const char* t = g;
            if (n > 6) return -1;
            for (int k = 0; k < 4; k++) {
                uint32_t x = 0; int dd = 0;
                while (*t >= '0' && *t <= '9') { x = x * 10 + (uint32_t)(*t++ - '0'); if (++dd > 3) return -1; }
                if (!dd || x > 255) return -1;
                q[k] = x;
                if (k < 3) { if (*t != '.') return -1; t++; }
            }
            w[n++] = (uint16_t)(q[0] << 8 | q[1]); w[n++] = (uint16_t)(q[2] << 8 | q[3]);
            s = t; break;
        }
        if (n >= 8) return -1;
        w[n++] = (uint16_t)v;
        if (*s != ':') break;
        s++;
        if (*s == ':') {
            if (gap >= 0) return -1;
            gap = n; s++;
            if (at_end(*s)) break;
        } else if (at_end(*s)) return -1;                             /* одиночное «:» в конце */
    }
    if (!at_end(*s)) return -1;
    uint16_t full[8];
    for (int i = 0; i < 8; i++) full[i] = 0;
    if (gap < 0) {
        if (n != 8) return -1;
        for (int i = 0; i < 8; i++) full[i] = w[i];
    } else {
        if (n > 7) return -1;
        for (int i = 0; i < gap; i++) full[i] = w[i];
        for (int i = gap; i < n; i++) full[8 - (n - i)] = w[i];
    }
    for (int i = 0; i < 8; i++) put16(out + 2 * i, full[i]);
    return 0;
}

void ip6_format(const uint8_t a[16], char out[40]) {
    uint16_t w[8]; int best = -1, blen = 0;
    for (int i = 0; i < 8; i++) w[i] = get16(a + 2 * i);
    for (int i = 0; i < 8; ) {                                        /* самая длинная серия нулей, при равенстве - первая */
        if (w[i]) { i++; continue; }
        int j = i; while (j < 8 && !w[j]) j++;
        if (j - i > blen) { blen = j - i; best = i; }
        i = j;
    }
    if (blen < 2) best = -1;                                          /* одну группу нулей не сокращают (RFC 5952 п. 4.2.2) */
    static const char hx[] = "0123456789abcdef";
    char* o = out; int after_gap = 0;
    for (int i = 0; i < 8; i++) {
        if (i == best) { *o++ = ':'; *o++ = ':'; i += blen - 1; after_gap = 1; continue; }
        if (i && !after_gap) *o++ = ':';
        after_gap = 0;
        int started = 0;
        for (int sh = 12; sh >= 0; sh -= 4) {
            unsigned x = (w[i] >> sh) & 15u;
            if (x || started || sh == 0) { *o++ = hx[x]; started = 1; }
        }
    }
    *o = 0;
}

int ip6_eq(const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0; return 1; }
int ip6_prefix_eq(const uint8_t* a, const uint8_t* b, unsigned bits) {
    if (bits > 128) bits = 128;
    for (unsigned i = 0; i < bits / 8; i++) if (a[i] != b[i]) return 0;
    if (bits % 8) { uint8_t m = (uint8_t)(0xFF << (8 - bits % 8)); if ((a[bits / 8] ^ b[bits / 8]) & m) return 0; }
    return 1;
}
int ip6_is_unspec(const uint8_t* a) { for (int i = 0; i < 16; i++) if (a[i]) return 0; return 1; }
int ip6_is_mcast(const uint8_t* a) { return a[0] == 0xFF; }
int ip6_is_linklocal(const uint8_t* a) { return a[0] == 0xFE && (a[1] & 0xC0) == 0x80; }

void ip6_eui64(const uint8_t mac[6], uint8_t iid[8]) {
    iid[0] = (uint8_t)(mac[0] ^ 2); iid[1] = mac[1]; iid[2] = mac[2]; iid[3] = 0xFF; iid[4] = 0xFE;
    iid[5] = mac[3]; iid[6] = mac[4]; iid[7] = mac[5];
}
void ip6_linklocal(const uint8_t mac[6], uint8_t a[16]) {
    zero(a, 16); a[0] = 0xFE; a[1] = 0x80; ip6_eui64(mac, a + 8);
}
int ip6_slaac(const uint8_t prefix[16], unsigned plen, const uint8_t mac[6], uint8_t a[16]) {
    if (plen != 64) return -1;                                        /* идентификатор EUI-64 - 64 бита */
    cpy(a, prefix, 8); ip6_eui64(mac, a + 8);
    return 0;
}
void ip6_solicited(const uint8_t a[16], uint8_t out[16]) {
    zero(out, 16); out[0] = 0xFF; out[1] = 0x02; out[11] = 1; out[12] = 0xFF;
    out[13] = a[13]; out[14] = a[14]; out[15] = a[15];
}
void ip6_mcast_mac(const uint8_t a[16], uint8_t mac[6]) {
    mac[0] = 0x33; mac[1] = 0x33; mac[2] = a[12]; mac[3] = a[13]; mac[4] = a[14]; mac[5] = a[15];
}

/* ---------- контрольные суммы ---------- */
uint32_t net_sum(uint32_t acc, const void* d, uint32_t n) {
    const uint8_t* p = (const uint8_t*)d;
    uint64_t s = acc;
    for (uint32_t i = 0; i + 1 < n; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 32) s = (s & 0xFFFFFFFFu) + (s >> 32);
    return (uint32_t)s;
}
uint16_t net_sum_end(uint32_t acc) {
    while (acc >> 16) acc = (acc & 0xFFFF) + (acc >> 16);
    return (uint16_t)~acc;
}
uint16_t net_csum4(uint32_t src, uint32_t dst, uint8_t proto, const void* d, uint32_t n) {
    uint8_t ph[12];
    put32(ph, src); put32(ph + 4, dst); ph[8] = 0; ph[9] = proto; put16(ph + 10, (uint16_t)n);
    return net_sum_end(net_sum(net_sum(0, ph, 12), d, n));
}
uint16_t net_csum6(const uint8_t* src, const uint8_t* dst, uint8_t proto, const void* d, uint32_t n) {
    uint8_t ph[40];
    cpy(ph, src, 16); cpy(ph + 16, dst, 16); put32(ph + 32, n); ph[36] = ph[37] = ph[38] = 0; ph[39] = proto;
    return net_sum_end(net_sum(net_sum(0, ph, 40), d, n));
}

/* ---------- заголовки расширения ---------- */
int ip6_walk(const uint8_t* b, uint32_t len, uint8_t nh, uint8_t* proto, uint32_t* off) {
    uint32_t o = 0;
    for (int hops = 0; hops < 8; hops++) {
        if (nh == 0 || nh == 60 || nh == 43) {                        /* параметры, получатель, маршрутизация */
            if (o + 8 > len) return -1;
            if (nh == 43 && b[o + 3] != 0) return -1;                 /* остались узлы маршрута: мы не пересылаем */
            uint32_t hl = ((uint32_t)b[o + 1] + 1) * 8;
            if (o + hl > len) return -1;
            nh = b[o]; o += hl;
        } else if (nh == 44) {
            if (o + 8 > len) return -1;
            *off = o; return 1;
        } else if (nh == 51 || nh == 50) {                            /* AH, ESP: безопасности IP нет */
            return -1;
        } else {
            *proto = nh; *off = o; return 0;
        }
    }
    return -1;
}

/* ---------- ND ---------- */
static uint32_t finish(uint8_t* o, uint32_t n, const uint8_t* src, const uint8_t* dst) {
    put16(o + 2, 0);
    put16(o + 2, net_csum6(src, dst, 58, o, n));
    return n;
}
static uint32_t opt_ll(uint8_t* o, uint32_t n, uint8_t type, const uint8_t* mac) {
    o[n] = type; o[n + 1] = 1; cpy(o + n + 2, mac, 6);
    return n + 8;
}
uint32_t nd_build_ns(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* target, const uint8_t* slla) {
    zero(o, 24); o[0] = 135; cpy(o + 8, target, 16);
    uint32_t n = 24;
    if (slla) n = opt_ll(o, n, 1, slla);
    return finish(o, n, src, dst);
}
uint32_t nd_build_na(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* target, uint8_t flags, const uint8_t* tlla) {
    zero(o, 24); o[0] = 136; o[4] = flags; cpy(o + 8, target, 16);
    uint32_t n = 24;
    if (tlla) n = opt_ll(o, n, 2, tlla);
    return finish(o, n, src, dst);
}
uint32_t nd_build_rs(uint8_t* o, const uint8_t* src, const uint8_t* dst, const uint8_t* slla) {
    zero(o, 8); o[0] = 133;
    uint32_t n = 8;
    if (slla) n = opt_ll(o, n, 1, slla);
    return finish(o, n, src, dst);
}
uint32_t icmp6_build_echo(uint8_t* o, const uint8_t* src, const uint8_t* dst, uint8_t type, uint16_t id, uint16_t seq,
                          const uint8_t* data, uint32_t n) {
    o[0] = type; o[1] = 0; put16(o + 2, 0); put16(o + 4, id); put16(o + 6, seq);
    cpy(o + 8, data, n);
    return finish(o, 8 + n, src, dst);
}

int nd_lladdr(const uint8_t* o, uint32_t n, uint8_t type, uint8_t mac[6]) {
    int found = 0;
    for (uint32_t i = 0; i < n; ) {
        if (i + 2 > n || o[i + 1] == 0 || i + (uint32_t)o[i + 1] * 8 > n) return -1;
        if (o[i] == type && o[i + 1] == 1 && !found) { cpy(mac, o + i + 2, 6); found = 1; }
        i += (uint32_t)o[i + 1] * 8;
    }
    return found;
}

int nd_parse_ra(const uint8_t* m, uint32_t n, ra_t* r) {
    if (n < 16 || m[0] != 134) return -1;
    zero(r, sizeof *r);
    r->hop = m[4]; r->flags = m[5]; r->lifetime = get16(m + 6); r->reachable = get32(m + 8); r->retrans = get32(m + 12);
    for (uint32_t i = 16; i < n; ) {
        if (i + 2 > n || m[i + 1] == 0 || i + (uint32_t)m[i + 1] * 8 > n) return -1;
        const uint8_t* o = m + i; uint32_t ol = (uint32_t)o[1] * 8;
        if (o[0] == 1 && o[1] == 1 && !r->have_slla) { cpy(r->slla, o + 2, 6); r->have_slla = 1; }
        else if (o[0] == 3 && o[1] == 4) {
            if (r->npi < RA_PI_MAX) {
                ra_pi_t* p = &r->pi[r->npi++];
                p->plen = o[2]; p->flags = o[3]; p->valid = get32(o + 4); p->pref = get32(o + 8); cpy(p->prefix, o + 16, 16);
            }
        } else if (o[0] == 5 && o[1] == 1) r->mtu = (uint16_t)(get32(o + 4) > 65535 ? 65535 : get32(o + 4));
        else if (o[0] == 25 && o[1] >= 3 && (o[1] & 1) && !r->have_dns) {
            r->dns_life = get32(o + 4); cpy(r->dns, o + 8, 16); r->have_dns = 1;
        }
        i += ol;
    }
    return 0;
}
