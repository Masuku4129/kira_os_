/* Сетевой стек ядра: Ethernet, ARP, IPv4 (со сборкой фрагментов и разбиением при отправке), ICMP (эхо: ответ на ping и
   свой ping), UDP, TCP, клиент DHCP, DNS (A) и IPv6: адрес канала из MAC (EUI-64) с проверкой повтора (DAD), Neighbor
   Discovery (NS/NA), Router Advertisement и SLAAC, ICMPv6 эхо, UDP и TCP по IPv6 (суммы с псевдозаголовком, ip6.c).
   Входящие кадры, DHCP и таймеры IPv6 обрабатывает поток «net» (опрос карты каждый тик); консоль отправляет ping и ждёт
   ответа, который отмечает поток. */
#include "net.h"
#include "ip6.h"
#include "ipfrag.h"

uint16_t net_csum(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t s = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) s += (uint32_t)p[len - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

int net_parse_ip(const char* s, uint32_t* ip) {
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t x = 0; int digits = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (uint32_t)(*s++ - '0'); if (++digits > 3) return -1; }
        if (x > 255) return -1;
        v = v << 8 | x;
        if (part < 3) { if (*s != '.') return -1; s++; }
    }
    if (*s && *s != ' ') return -1;
    *ip = v;
    return 0;
}

#ifdef KIRA_HOST_TEST
/* хост: стека нет (тест консоли видит команды, тест сети - чистые функции) */
void netcmd_net(const char* a) { (void)a; }
void netcmd_ping(const char* a) { (void)a; }
void netcmd_dns(const char* a) { (void)a; }
void netcmd_http(const char* a) { (void)a; }
void netcmd_ping6(const char* a) { (void)a; }
void net_proc_gone(uint32_t pid) { (void)pid; }
#else
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "uart.h"
#include "spinlock.h"
#include "msi.h"
#include "heap.h"

static netdev_t* dev;
static uint32_t my_ip, my_mask, my_gw, my_dns, lease_s;
static volatile uint64_t rx_frames, tx_frames, rx_drop;
static spinlock_t lk = SPINLOCK_INIT;

#define ARP_N 16
static struct { uint32_t ip; uint8_t mac[6]; uint8_t valid; uint64_t when; } arp[ARP_N];

/* ping: что ждём и что пришло (поток «net» заполняет, консоль читает) */
static volatile uint16_t ping_seq;
static volatile uint32_t ping_ip;
static volatile int ping_got;
static volatile uint64_t ping_rx_ns;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void cpy(void* d, const void* s, uint32_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }

static void print_ip(uint32_t ip) {
    for (int i = 3; i >= 0; i--) { uart_print_dec((ip >> (8 * i)) & 255); if (i) uart_print("."); }
}
static void print_ip6(const uint8_t* a) { char s[40]; ip6_format(a, s); uart_print(s); }

/* Адрес узла любого семейства: fam 4 - в a[0..3], fam 6 - a[0..15]. */
typedef struct { uint8_t fam; uint8_t a[16]; } ipa_t;
static ipa_t ipa_v4(uint32_t v) { ipa_t r; for (int i = 0; i < 16; i++) r.a[i] = 0; r.fam = 4; put32(r.a, v); return r; }
static ipa_t ipa_v6(const uint8_t* a) { ipa_t r; r.fam = 6; cpy(r.a, a, 16); return r; }
static uint32_t ipa_to4(const ipa_t* x) { return get32(x->a); }
static int ipa_eq(const ipa_t* x, const ipa_t* y) {
    if (x->fam != y->fam) return 0;
    for (int i = 0; i < (x->fam == 4 ? 4 : 16); i++) if (x->a[i] != y->a[i]) return 0;
    return 1;
}

static int eth_send(const uint8_t* dst, uint16_t type, const uint8_t* payload, uint32_t len) {
    uint8_t f[1514];
    if (!dev || len > 1500) return -1;
    cpy(f, dst, 6); cpy(f + 6, dev->mac, 6); put16(f + 12, type);
    cpy(f + 14, payload, len);
    int r = dev->send(dev, f, 14 + len);
    if (r == 0) tx_frames++;
    return r;
}

/* ---------- ARP ---------- */
static void arp_learn(uint32_t ip, const uint8_t* mac) {
    if (!ip) return;
    uint64_t f = spin_lock_irqsave(&lk);
    int slot = -1;
    for (int i = 0; i < ARP_N && slot < 0; i++) if (arp[i].valid && arp[i].ip == ip) slot = i;     /* уже знаем */
    for (int i = 0; i < ARP_N && slot < 0; i++) if (!arp[i].valid) slot = i;                      /* свободная */
    if (slot < 0) { slot = 0; for (int i = 1; i < ARP_N; i++) if (arp[i].when < arp[slot].when) slot = i; }   /* старейшая */
    arp[slot].ip = ip; cpy(arp[slot].mac, mac, 6); arp[slot].valid = 1; arp[slot].when = timer_get_ticks();
    spin_unlock_irqrestore(&lk, f);
}
static int arp_lookup(uint32_t ip, uint8_t* mac) {
    uint64_t f = spin_lock_irqsave(&lk);
    int ok = 0;
    for (int i = 0; i < ARP_N; i++) if (arp[i].valid && arp[i].ip == ip) { cpy(mac, arp[i].mac, 6); ok = 1; break; }
    spin_unlock_irqrestore(&lk, f);
    return ok;
}
static void arp_send(uint16_t op, const uint8_t* tmac, uint32_t tip, const uint8_t* dst) {
    uint8_t a[28];
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, op);
    cpy(a + 8, dev->mac, 6); put32(a + 14, my_ip);
    cpy(a + 18, tmac, 6); put32(a + 24, tip);
    eth_send(dst, 0x0806, a, 28);
}
static void arp_input(const uint8_t* a, uint32_t len) {
    if (len < 28 || get16(a) != 1 || get16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4) return;
    uint32_t sip = get32(a + 14), tip = get32(a + 24);
    arp_learn(sip, a + 8);
    if (get16(a + 6) == 1 && my_ip && tip == my_ip) arp_send(2, a + 8, sip, a + 8);   /* кто-то ищет нас - ответить */
}

/* Адрес следующего узла: в своей сети - сам получатель, иначе шлюз. Ждёт ответа ARP до ~1 с (только не поток «net»). */
static int resolve(uint32_t ip, uint8_t* mac) {
    if (ip == 0xFFFFFFFF) { cpy(mac, BCAST, 6); return 0; }
    uint32_t hop = (my_mask && (ip & my_mask) == (my_ip & my_mask)) ? ip : my_gw;
    if (!hop) return -1;
    for (int tries = 0; tries < 4; tries++) {
        if (arp_lookup(hop, mac)) return 0;
        arp_send(1, (const uint8_t*)"\0\0\0\0\0\0", hop, BCAST);
        for (uint32_t t = 0; t < TIMER_HZ / 4; t++) { if (arp_lookup(hop, mac)) return 0; sched_sleep_ticks(1); }
    }
    return -1;
}

/* ---------- IPv4 ---------- */
static uint16_t ip_ident;
/* Отправка датаграммы IPv4. Не больше 1480 байт данных - одним кадром (DF); больше - фрагментами по 1480 (кратно 8,
   RFC 791), общий идентификатор, MF у всех кроме последнего. */
static int ip_send(const uint8_t* mac, uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* data, uint32_t len) {
    uint8_t p[1500];
    if (len > 65515) return -1;
    uint16_t id = __atomic_fetch_add(&ip_ident, 1, __ATOMIC_RELAXED);
    uint32_t off = 0;
    do {
        uint32_t n = len - off; int more = 0;
        if (n > 1480) { n = 1480; more = 1; }
        p[0] = 0x45; p[1] = 0; put16(p + 2, (uint16_t)(20 + n)); put16(p + 4, id);
        put16(p + 6, len <= 1480 ? 0x4000 : (uint16_t)((more ? 0x2000 : 0) | (off / 8)));
        p[8] = 64; p[9] = proto; put16(p + 10, 0); put32(p + 12, src); put32(p + 16, dst);
        put16(p + 10, net_csum(p, 20));
        cpy(p + 20, data + off, n);
        int r = eth_send(mac, 0x0800, p, 20 + n);
        if (r) return r;
        off += n;
    } while (off < len);
    return 0;
}

/* ---------- IPv6: адреса, соседи, маршрутизатор ----------
   Адрес канала (fe80::/64 + EUI-64 из MAC) создаётся при появлении канала. Каждый адрес сначала «пробный» (tentative):
   одна проверка повтора (DAD, RFC 4862: NS с адреса «::» на solicited-node адрес, ждём 1 с), если никто не возразил -
   «рабочий». Потом - до трёх Router Solicitation (через 4 с); по Router Advertisement: префикс с флагом A (и длиной 64) даёт
   адрес SLAAC (тоже с DAD), с флагом L - «на канале», срок маршрутизатора > 0 - маршрутизатор по умолчанию. Соседи
   (кэш из 16 записей) узнаются по NS/NA; запись живёт 5 минут и потом спрашивается заново (проверки достижимости NUD
   по RFC 4861 п. 7.3 нет). Состояние под замком lk6; кадры уходят без замка. */
#define A6_MAX 4
#define ND_MAX 16
#define PFX_MAX 4
#define ND_LIFE (300 * TIMER_HZ)
enum { A6_FREE, A6_TENT, A6_PREF };
static struct { uint8_t a[16], state, plen, ll, dad_sent; uint64_t valid_until, dad_at; } a6[A6_MAX];   /* valid_until 0 - бессрочно */
static struct { uint8_t p[16], plen; uint64_t until; } pfx6[PFX_MAX];                                      /* префиксы «на канале» */
static uint8_t gw6[16], gw6_set, dns6[16], dns6_set;
static uint64_t gw6_until;
static unsigned mtu6 = 1500;
static volatile int v6_on, v6_dup, ra_seen;
static uint64_t rs_at, rs_end; static unsigned rs_left;
static struct { uint8_t ip[16], mac[6], st; uint64_t when; } nd6[ND_MAX];                                  /* st: 0 нет, 1 ждём ответ, 2 известен */
static spinlock_t lk6 = SPINLOCK_INIT;
static volatile uint64_t v6_drop;
static const uint8_t ALL_NODES[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static const uint8_t ALL_ROUTERS[16] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };

static int mac_eq(const uint8_t* a, const uint8_t* b) { for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0; return 1; }

static void mc_join(const uint8_t* maddr) {
    uint8_t m[6];
    ip6_mcast_mac(maddr, m);
    if (dev && dev->mcast_add) dev->mcast_add(dev, m);
}

static int ip6_send(const uint8_t* mac, const uint8_t* src, const uint8_t* dst, uint8_t nh, uint8_t hop, const uint8_t* data, uint32_t len) {
    uint8_t p[1500];
    if (!dev || len + 40 > mtu6) return -1;
    p[0] = 0x60; p[1] = p[2] = p[3] = 0; put16(p + 4, (uint16_t)len); p[6] = nh; p[7] = hop;
    cpy(p + 8, src, 16); cpy(p + 24, dst, 16); cpy(p + 40, data, len);
    return eth_send(mac, 0x86DD, p, 40 + len);
}

/* Отправка датаграммы транспорта по IPv4 или IPv6 (mac - уже найденный следующий узел). */
static int ipa_send(const uint8_t* mac, const ipa_t* src, const ipa_t* dst, uint8_t proto, const uint8_t* data, uint32_t len) {
    if (dst->fam == 4) return ip_send(mac, ipa_to4(src), ipa_to4(dst), proto, data, len);
    return ip6_send(mac, src->a, dst->a, proto, 64, data, len);
}

/* кэш соседей (замок lk6 взят) */
static int nd_find(const uint8_t* ip) { for (int i = 0; i < ND_MAX; i++) if (nd6[i].st && ip6_eq(nd6[i].ip, ip)) return i; return -1; }
static void nd_put(const uint8_t* ip, const uint8_t* mac, int st, uint64_t now) {
    int s = nd_find(ip);
    if (s < 0) for (int i = 0; i < ND_MAX && s < 0; i++) if (!nd6[i].st) s = i;
    if (s < 0) { s = 0; for (int i = 1; i < ND_MAX; i++) if (nd6[i].when < nd6[s].when) s = i; }
    cpy(nd6[s].ip, ip, 16);
    if (mac) cpy(nd6[s].mac, mac, 6);
    nd6[s].st = (uint8_t)st; nd6[s].when = now;
}
static void nd_learn(const uint8_t* ip, const uint8_t* mac) {
    uint64_t f = spin_lock_irqsave(&lk6);
    nd_put(ip, mac, 2, timer_get_ticks());
    spin_unlock_irqrestore(&lk6, f);
}
static int nd_lookup(const uint8_t* ip, uint8_t* mac) {
    uint64_t now = timer_get_ticks(), f = spin_lock_irqsave(&lk6);
    int s = nd_find(ip), ok = s >= 0 && nd6[s].st == 2 && now - nd6[s].when < ND_LIFE;
    if (ok) cpy(mac, nd6[s].mac, 6);
    spin_unlock_irqrestore(&lk6, f);
    return ok;
}

static int a6_find(const uint8_t* a) { for (int i = 0; i < A6_MAX; i++) if (a6[i].state != A6_FREE && ip6_eq(a6[i].a, a)) return i; return -1; }
static int a6_state(const uint8_t* a) {
    uint64_t f = spin_lock_irqsave(&lk6);
    int i = a6_find(a), st = i < 0 ? A6_FREE : a6[i].state;
    spin_unlock_irqrestore(&lk6, f);
    return st;
}
/* Адрес solicited-node одного из наших адресов? */
static int is_our_solicited(const uint8_t* d) {
    uint8_t s[16]; int r = 0;
    uint64_t f = spin_lock_irqsave(&lk6);
    for (int i = 0; i < A6_MAX && !r; i++) if (a6[i].state != A6_FREE) { ip6_solicited(a6[i].a, s); r = ip6_eq(s, d); }
    spin_unlock_irqrestore(&lk6, f);
    return r;
}

/* Добавить пробный адрес (ll - адрес канала). */
static int a6_add(const uint8_t* a, uint8_t plen, int ll, uint64_t valid_until, uint64_t now) {
    int slot = -1;
    uint64_t f = spin_lock_irqsave(&lk6);
    for (int i = 0; i < A6_MAX && slot < 0; i++) if (a6[i].state == A6_FREE) slot = i;
    if (slot >= 0) {
        cpy(a6[slot].a, a, 16); a6[slot].plen = plen; a6[slot].ll = (uint8_t)ll; a6[slot].state = A6_TENT;
        a6[slot].dad_sent = 0; a6[slot].valid_until = valid_until;
        a6[slot].dad_at = now + (clock_ns() >> 10) % TIMER_HZ;      /* случайная задержка до 1 с (RFC 4862 п. 5.4.2) */
    }
    spin_unlock_irqrestore(&lk6, f);
    if (slot >= 0) { uint8_t s[16]; ip6_solicited(a, s); mc_join(s); }
    return slot;
}

/* Выбрать адрес-источник для получателя; -1 - рабочего адреса нет. */
static int src6_for(const uint8_t* dst, uint8_t* out) {
    int ll = -1, gl = -1;
    uint64_t f = spin_lock_irqsave(&lk6);
    for (int i = 0; i < A6_MAX; i++) {
        if (a6[i].state != A6_PREF) continue;
        if (a6[i].ll) ll = i;
        else if (gl < 0 || ip6_prefix_eq(a6[i].a, dst, a6[i].plen)) gl = i;
    }
    int link_scope = ip6_is_linklocal(dst) || (ip6_is_mcast(dst) && (dst[1] & 15) <= 2);
    int pick = link_scope ? ll : (gl >= 0 ? gl : ll);
    if (pick >= 0) cpy(out, a6[pick].a, 16);
    spin_unlock_irqrestore(&lk6, f);
    return pick < 0 ? -1 : 0;
}

static int onlink(const uint8_t* dst, uint64_t now) {         /* замок взят */
    for (int i = 0; i < PFX_MAX; i++) if (pfx6[i].plen && pfx6[i].until > now && ip6_prefix_eq(dst, pfx6[i].p, pfx6[i].plen)) return 1;
    return 0;
}

static void nd_send_ns(const uint8_t* src, const uint8_t* target, int dad) {
    uint8_t m[32], dst[16], mac[6], un[16];
    for (int i = 0; i < 16; i++) un[i] = 0;
    ip6_solicited(target, dst); ip6_mcast_mac(dst, mac);
    const uint8_t* s = dad ? un : src;
    uint32_t n = nd_build_ns(m, s, dst, target, dad ? 0 : dev->mac);
    ip6_send(mac, s, dst, 58, 255, m, n);
}
static void nd_send_na(const uint8_t* mac, const uint8_t* src, const uint8_t* dst, const uint8_t* target, uint8_t flags) {
    uint8_t m[32];
    uint32_t n = nd_build_na(m, src, dst, target, flags, dev->mac);
    ip6_send(mac, src, dst, 58, 255, m, n);
}

/* Следующий узел для получателя: на канале - сам, иначе маршрутизатор по умолчанию; ждёт ответа NS до 3 с. */
static int resolve6(const uint8_t* dst, uint8_t* mac) {
    if (!dev || !v6_on) return -1;
    if (ip6_is_mcast(dst)) { ip6_mcast_mac(dst, mac); return 0; }
    uint8_t hop[16], src[16];
    uint64_t now = timer_get_ticks(), f = spin_lock_irqsave(&lk6);
    if (ip6_is_linklocal(dst) || onlink(dst, now)) cpy(hop, dst, 16);
    else if (gw6_set && now < gw6_until) cpy(hop, gw6, 16);
    else { spin_unlock_irqrestore(&lk6, f); return -1; }
    spin_unlock_irqrestore(&lk6, f);
    if (src6_for(hop, src) != 0) return -1;
    for (int tries = 0; tries < 3; tries++) {
        if (nd_lookup(hop, mac)) return 0;
        f = spin_lock_irqsave(&lk6);
        if (nd_find(hop) < 0) nd_put(hop, 0, 1, timer_get_ticks());   /* ждём ответ: только на такой NA верим */
        spin_unlock_irqrestore(&lk6, f);
        nd_send_ns(src, hop, 0);
        for (uint32_t t = 0; t < TIMER_HZ; t++) { if (nd_lookup(hop, mac)) return 0; sched_sleep_ticks(1); }
    }
    return -1;
}

static void v6_down(void) {
    uint64_t f = spin_lock_irqsave(&lk6);
    for (int i = 0; i < A6_MAX; i++) a6[i].state = A6_FREE;
    for (int i = 0; i < ND_MAX; i++) nd6[i].st = 0;
    for (int i = 0; i < PFX_MAX; i++) pfx6[i].plen = 0;
    gw6_set = 0; dns6_set = 0; mtu6 = 1500; rs_left = 0; rs_end = 0; ra_seen = 0;
    spin_unlock_irqrestore(&lk6, f);
    v6_on = 0;
}

static void v6_up(uint64_t now) {
    v6_down();
    v6_dup = 0;
    uint8_t ll[16];
    ip6_linklocal(dev->mac, ll);
    mc_join(ALL_NODES);
    v6_on = 1;
    a6_add(ll, 64, 1, 0, now);
}

/* Адрес оказался чужим (DAD): адрес канала - IPv6 выключается, иначе уходит только этот адрес. */
static void a6_conflict(const uint8_t* a) {
    uint8_t c[16];
    cpy(c, a, 16);
    uint64_t f = spin_lock_irqsave(&lk6);
    int i = a6_find(c), ll = 0;
    if (i >= 0) { ll = a6[i].ll; a6[i].state = A6_FREE; }
    spin_unlock_irqrestore(&lk6, f);
    if (i < 0) return;
    uart_print("[KIRA NET] IPv6: address "); print_ip6(c); uart_print(" is already used by another node (DAD)");
    if (ll) { uart_print(" - IPv6 off on this link\n"); v6_down(); v6_dup = 1; } else uart_print(" - dropped\n");
}

/* Таймеры IPv6 (поток «net», каждый тик): DAD, сроки адресов, Router Solicitation. */
static void v6_tick(uint64_t now) {
    if (!v6_on) return;
    for (int i = 0; i < A6_MAX; i++) {
        uint8_t a[16]; int act = 0, ll = 0, plen = 0;
        uint64_t f = spin_lock_irqsave(&lk6);
        if (a6[i].state == A6_TENT && now >= a6[i].dad_at) {
            cpy(a, a6[i].a, 16); ll = a6[i].ll; plen = a6[i].plen;
            if (a6[i].dad_sent < 1) { a6[i].dad_sent++; a6[i].dad_at = now + TIMER_HZ; act = 1; }      /* одна проба, ждать 1 с */
            else { a6[i].state = A6_PREF; act = 2; }
        } else if (a6[i].state != A6_FREE && a6[i].valid_until && now >= a6[i].valid_until) {
            cpy(a, a6[i].a, 16); a6[i].state = A6_FREE; act = 3;
        }
        spin_unlock_irqrestore(&lk6, f);
        if (act == 1) nd_send_ns(a, a, 1);
        else if (act == 2) {
            uart_print("[KIRA NET] IPv6: "); print_ip6(a);
            if (ll) uart_print(" (link-local, DAD passed) ready\n");
            else { uart_print(" (SLAAC /"); uart_print_dec((uint64_t)plen); uart_print(", DAD passed) ready\n"); }
            if (ll) { rs_left = 3; rs_at = now + (clock_ns() >> 10) % TIMER_HZ; }
        } else if (act == 3) { uart_print("[KIRA NET] IPv6: address "); print_ip6(a); uart_print(" expired\n"); }
    }
    if (rs_left && now >= rs_at) {
        uint8_t src[16];
        if (src6_for(ALL_ROUTERS, src) == 0) {
            uint8_t m[16], mac[6];
            ip6_mcast_mac(ALL_ROUTERS, mac);
            uint32_t n = nd_build_rs(m, src, ALL_ROUTERS, dev->mac);
            ip6_send(mac, src, ALL_ROUTERS, 58, 255, m, n);
            rs_left--; rs_at = now + 4 * TIMER_HZ;
            if (!rs_left) rs_end = rs_at;
        }
    }
    if (rs_end && now >= rs_end) {
        rs_end = 0;
        if (!ra_seen) uart_print("[KIRA NET] IPv6: no router advertisement after 3 solicitations - link-local only\n");
    }
}

/* ---------- ICMPv6: эхо, ND ---------- */
static volatile uint16_t ping6_seq;
static uint8_t ping6_ip[16];
static volatile int ping6_got;
static volatile uint64_t ping6_rx_ns;

static void ra_input(const uint8_t* eth_src, const uint8_t* src, const ra_t* r, uint64_t now) {
    uint8_t rmac[6];
    cpy(rmac, r->have_slla ? r->slla : eth_src, 6);
    uint64_t f = spin_lock_irqsave(&lk6);
    if (r->lifetime) {
        cpy(gw6, src, 16); gw6_set = 1; gw6_until = now + (uint64_t)r->lifetime * TIMER_HZ;
        nd_put(src, rmac, 2, now);
    } else if (gw6_set && ip6_eq(gw6, src)) gw6_set = 0;
    if (r->mtu >= 1280) mtu6 = r->mtu < 1500 ? r->mtu : 1500;
    if (r->have_dns) { cpy(dns6, r->dns, 16); dns6_set = 1; }
    spin_unlock_irqrestore(&lk6, f);
    if (!ra_seen) { uart_print("[KIRA NET] IPv6: router advertisement from "); print_ip6(src); uart_print(r->lifetime ? ", default router\n" : ", not a default router\n"); }
    ra_seen = 1; rs_left = 0; rs_end = 0;
    for (int k = 0; k < r->npi; k++) {
        const ra_pi_t* p = &r->pi[k];
        if (p->plen > 128 || ip6_is_linklocal(p->prefix) || ip6_is_mcast(p->prefix) || p->pref > p->valid) continue;
        uint64_t until = p->valid == 0xFFFFFFFFu ? 0 : now + (uint64_t)p->valid * TIMER_HZ;
        if ((p->flags & RA_PI_ONLINK) && p->plen) {
            f = spin_lock_irqsave(&lk6);
            int s = -1;
            for (int i = 0; i < PFX_MAX; i++) if (pfx6[i].plen == p->plen && ip6_prefix_eq(pfx6[i].p, p->prefix, p->plen)) s = i;
            for (int i = 0; i < PFX_MAX && s < 0; i++) if (!pfx6[i].plen || pfx6[i].until <= now) s = i;
            if (s >= 0) {
                cpy(pfx6[s].p, p->prefix, 16); pfx6[s].plen = p->valid ? p->plen : 0; pfx6[s].until = until ? until : ~0ull;
            }
            spin_unlock_irqrestore(&lk6, f);
        }
        if (p->flags & RA_PI_AUTO) {
            uint8_t a[16];
            if (ip6_slaac(p->prefix, p->plen, dev->mac, a) != 0) {
                uart_print("[KIRA NET] IPv6: prefix length "); uart_print_dec(p->plen); uart_print(" is not 64 - no SLAAC (RFC 4862)\n");
                continue;
            }
            f = spin_lock_irqsave(&lk6);
            int i = a6_find(a);
            if (i >= 0 && !a6[i].ll) {                              /* уже есть: срок по правилу RFC 4862 п. 5.5.3 e */
                uint64_t rem = a6[i].valid_until ? (a6[i].valid_until > now ? a6[i].valid_until - now : 0) : ~0ull;
                if (p->valid > 2 * 3600 || (uint64_t)p->valid * TIMER_HZ > rem) a6[i].valid_until = until;
                else if (rem > 2ull * 3600 * TIMER_HZ) a6[i].valid_until = now + 2ull * 3600 * TIMER_HZ;
            }
            spin_unlock_irqrestore(&lk6, f);
            if (i < 0 && p->valid) a6_add(a, p->plen, 0, until, now);
        }
    }
}

/* ds: состояние нашего адреса получателя (A6_*), 0 - получатель групповой. */
static void icmp6_input(const uint8_t* eth_src, const uint8_t* src, const uint8_t* dst, uint8_t hop, int ds, const uint8_t* m, uint32_t len) {
    if (len < 8 || net_csum6(src, dst, 58, m, len) != 0) return;
    uint64_t now = timer_get_ticks();
    uint8_t type = m[0];
    if (type == 128) {                                              /* эхо-запрос: ответить на рабочий адрес или на «все узлы» */
        uint8_t s[16], r[1500];
        if (ds == A6_PREF) cpy(s, dst, 16);
        else if (ds == 0 && ip6_eq(dst, ALL_NODES)) { if (src6_for(src, s) != 0) return; }
        else return;
        if (len + 40 > mtu6 || ip6_is_unspec(src)) return;       /* ответ по частям IPv6 не отправляется */
        uint32_t n = icmp6_build_echo(r, s, src, 129, get16(m + 4), get16(m + 6), m + 8, len - 8);
        ip6_send(eth_src, s, src, 58, 64, r, n);
    } else if (type == 129) {
        if (get16(m + 4) == 0x4B49 && get16(m + 6) == ping6_seq && ip6_eq(src, ping6_ip)) {
            ping6_rx_ns = clock_ns();
            __atomic_store_n(&ping6_got, 1, __ATOMIC_RELEASE);
        }
    } else if (type == 134) {                                       /* Router Advertisement */
        ra_t r;
        if (hop != 255 || m[1] != 0 || !ip6_is_linklocal(src) || nd_parse_ra(m, len, &r) != 0) { v6_drop++; return; }
        ra_input(eth_src, src, &r, now);
    } else if (type == 135 || type == 136) {                        /* NS / NA */
        uint8_t mac[6], target[16];
        if (hop != 255 || m[1] != 0 || len < 24 || ip6_is_mcast(m + 8)) { v6_drop++; return; }
        int ll = nd_lladdr(m + 24, len - 24, type == 135 ? 1 : 2, mac);
        if (ll < 0) { v6_drop++; return; }
        cpy(target, m + 8, 16);
        int ai = a6_state(target);
        if (type == 135) {
            if (ai == A6_FREE) return;                             /* не про нас (маршрутизатором не работаем) */
            if (ip6_is_unspec(src)) {                              /* проверка повтора (DAD) другим узлом */
                if (ll || !is_our_solicited(dst)) return;          /* без опции адреса и на solicited-node (RFC 4861 п. 7.1.1) */
                if (ai == A6_TENT) a6_conflict(target);            /* оба проверяют один адрес */
                else nd_send_na(eth_src, target, ALL_NODES, target, ND_NA_O);   /* адрес занят нами: сказать всем */
                return;
            }
            if (ll) nd_learn(src, mac);
            if (ai == A6_TENT) return;                             /* пробный адрес не отвечает (RFC 4862 п. 5.4.3) */
            nd_send_na(ll ? mac : eth_src, target, src, target, ND_NA_S | ND_NA_O);
        } else {
            if (ai == A6_TENT) { a6_conflict(target); return; }   /* кто-то отвечает за наш пробный адрес */
            uint64_t f = spin_lock_irqsave(&lk6);
            int s = nd_find(target);
            if (s >= 0) {                                          /* верим только тем, кого спрашивали или знаем */
                if (ll == 1 && (nd6[s].st == 1 || mac_eq(nd6[s].mac, mac) || (m[4] & ND_NA_O))) {
                    cpy(nd6[s].mac, mac, 6); nd6[s].st = 2; nd6[s].when = now;
                } else if (ll == 0 && nd6[s].st == 2) nd6[s].when = now;
            }
            spin_unlock_irqrestore(&lk6, f);
        }
    }
}

/* ---------- DHCP ---------- */
enum { D_INIT, D_SELECTING, D_REQUESTING, D_BOUND };
static int dstate;
static uint32_t xid, offer_ip, offer_srv;
static uint64_t dnext, dbound_at;
static unsigned dtries;

static void dhcp_send(int type) {
    uint8_t u[8 + 300];
    uint8_t* b = u + 8;
    for (unsigned i = 0; i < sizeof u; i++) u[i] = 0;
    b[0] = 1; b[1] = 1; b[2] = 6; put32(b + 4, xid); put16(b + 10, 0x8000);   /* ответ - широковещательно */
    cpy(b + 28, dev->mac, 6);
    put32(b + 236, 0x63825363);
    uint8_t* o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (type == 3) { *o++ = 50; *o++ = 4; put32(o, offer_ip); o += 4; *o++ = 54; *o++ = 4; put32(o, offer_srv); o += 4; }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;              /* нужны: маска, шлюз, DNS */
    *o++ = 255;
    put16(u, 68); put16(u + 2, 67); put16(u + 4, (uint16_t)sizeof u); put16(u + 6, 0);
    ip_send(BCAST, 0, 0xFFFFFFFF, 17, u, sizeof u);
}

static void dhcp_input(const uint8_t* b, uint32_t len) {
    if (len < 240 || b[0] != 2 || get32(b + 4) != xid || get32(b + 236) != 0x63825363) return;
    int type = 0; uint32_t mask = 0, gw = 0, dns = 0, srv = 0, lease = 0;
    for (uint32_t i = 240; i < len && b[i] != 255; ) {
        if (b[i] == 0) { i++; continue; }
        if (i + 1 >= len || i + 2 + b[i + 1] > len) break;
        uint8_t code = b[i], n = b[i + 1]; const uint8_t* v = b + i + 2;
        if (code == 53 && n >= 1) type = v[0];
        else if (code == 1 && n >= 4) mask = get32(v);
        else if (code == 3 && n >= 4) gw = get32(v);
        else if (code == 6 && n >= 4) dns = get32(v);
        else if (code == 54 && n >= 4) srv = get32(v);
        else if (code == 51 && n >= 4) lease = get32(v);
        i += 2u + n;
    }
    uint32_t yi = get32(b + 16);
    if (type == 2 && dstate == D_SELECTING) {                       /* OFFER -> REQUEST */
        offer_ip = yi; offer_srv = srv; dstate = D_REQUESTING; dtries = 0;
        dhcp_send(3); dnext = timer_get_ticks() + 2 * TIMER_HZ;
    } else if (type == 5 && dstate == D_REQUESTING) {               /* ACK: адрес наш */
        my_ip = yi; my_mask = mask ? mask : 0xFFFFFF00; my_gw = gw; my_dns = dns; lease_s = lease ? lease : 3600;
        dstate = D_BOUND; dbound_at = timer_get_ticks();
        unsigned bits = 0; for (uint32_t m = my_mask; m; m <<= 1) bits++;
        uart_print("[KIRA NET] DHCP: address "); print_ip(my_ip); uart_print("/"); uart_print_dec(bits);
        uart_print(", gateway "); print_ip(my_gw); uart_print(", DNS "); print_ip(my_dns);
        uart_print(", lease "); uart_print_dec(lease_s); uart_print(" s\n");
    } else if (type == 6 && dstate == D_REQUESTING) {               /* NAK - сначала */
        dstate = D_INIT; dnext = timer_get_ticks() + TIMER_HZ;
    }
}

static void dhcp_tick(uint64_t now) {
    if (!dev->link(dev)) return;
    if (dstate == D_BOUND) {                                        /* продление: в середине аренды - заново (тот же адрес) */
        if (now - dbound_at < (uint64_t)lease_s * TIMER_HZ / 2) return;
        dstate = D_INIT; dnext = now;
    }
    if (now < dnext) return;
    if (dstate == D_INIT || dtries >= 4) {
        xid = (uint32_t)(clock_ns() ^ (uint64_t)dev->mac[5] << 24 ^ now);
        dstate = D_SELECTING; dtries = 0;
    }
    dtries++;
    if (dstate == D_SELECTING) dhcp_send(1); else dhcp_send(3);
    dnext = now + (uint64_t)(2u << (dtries < 4 ? dtries : 4)) * TIMER_HZ / 2;   /* 2, 4, 8, 16 с */
}

/* ---------- DNS (клиент, записи A) ---------- */
static volatile uint16_t dns_port, dns_id;
static uint8_t dns_buf[512];
static volatile uint32_t dns_len;
static volatile int dns_got;

static void dns_input(const uint8_t* b, uint32_t len) {
    if (len < 12 || get16(b) != dns_id || len > sizeof dns_buf) return;
    cpy(dns_buf, b, len); dns_len = len;
    __atomic_store_n(&dns_got, 1, __ATOMIC_RELEASE);
}

/* Пропустить имя в ответе (метки или ссылка сжатия). Смещение после имени или 0 - ошибка. */
static uint32_t dns_skip_name(const uint8_t* b, uint32_t len, uint32_t i) {
    while (i < len) {
        uint8_t c = b[i];
        if (c == 0) return i + 1;
        if ((c & 0xC0) == 0xC0) return i + 2 <= len ? i + 2 : 0;
        if (c & 0xC0) return 0;
        i += 1u + c;
    }
    return 0;
}

int net_dns(const char* name, uint32_t* ip) {
    if (net_parse_ip(name, ip) == 0) return 0;                      /* уже адрес */
    if (!dev || dstate != D_BOUND || !my_dns) return -1;
    uint8_t mac[6];
    if (resolve(my_dns, mac) != 0) return -2;
    uint8_t u[8 + 12 + 256 + 4];
    uint8_t* q = u + 8;
    uint32_t n = 12;
    for (unsigned i = 0; i < sizeof u; i++) u[i] = 0;
    const char* s = name;
    while (*s) {                                                    /* «example.com» -> 7example3com0 */
        uint32_t k = 0;
        while (s[k] && s[k] != '.') k++;
        if (k == 0 || k > 63 || n + 1 + k + 5 > 12 + 256) return -3;
        q[n++] = (uint8_t)k;
        for (uint32_t j = 0; j < k; j++) q[n++] = (uint8_t)s[j];
        s += k;
        if (*s == '.') s++;
    }
    q[n++] = 0;
    put16(q + n, 1); put16(q + n + 2, 1); n += 4;                  /* тип A, класс IN */
    for (int attempt = 0; attempt < 3; attempt++) {
        dns_id = (uint16_t)(clock_ns() >> 4); dns_port = (uint16_t)(49152 + (clock_ns() >> 8) % 16000);
        put16(q, dns_id); put16(q + 2, 0x0100); put16(q + 4, 1);   /* рекурсия желательна, один вопрос */
        put16(u, dns_port); put16(u + 2, 53); put16(u + 4, (uint16_t)(8 + n)); put16(u + 6, 0);
        __atomic_store_n(&dns_got, 0, __ATOMIC_RELEASE);
        ip_send(mac, my_ip, my_dns, 17, u, 8 + n);
        uint64_t until = timer_get_ticks() + 2 * TIMER_HZ;
        while (!__atomic_load_n(&dns_got, __ATOMIC_ACQUIRE) && timer_get_ticks() < until) sched_sleep_ticks(1);
        if (!dns_got) continue;
        const uint8_t* b = dns_buf; uint32_t len = dns_len;
        if (!(b[2] & 0x80) || (b[3] & 15)) return -4;               /* не ответ или ошибка (нет такого имени) */
        uint32_t qd = get16(b + 4), an = get16(b + 6), i = 12;
        for (uint32_t k = 0; k < qd && i; k++) { i = dns_skip_name(b, len, i); if (i) i += 4; }
        for (uint32_t k = 0; k < an && i && i < len; k++) {
            i = dns_skip_name(b, len, i);
            if (!i || i + 10 > len) break;
            uint16_t type = get16(b + i), cls = get16(b + i + 2), rd = get16(b + i + 8);
            i += 10;
            if (i + rd > len) break;
            if (type == 1 && cls == 1 && rd == 4) { *ip = get32(b + i); return 0; }
            i += rd;                                                /* CNAME и прочее - дальше */
        }
        return -5;
    }
    return -6;
}

/* ---------- TCP ----------
   Соединения ядра - исходящие (tcp_connect) и входящие (tcp_listen + tcp_accept). Передача - окном: в пути сколько
   позволяет окно собеседника и кольцо отправки (16 КБ), сегментами по MSS; по сроку повторяется первый
   неподтверждённый сегмент (1, 2, 4... с, 8 попыток), три повторных подтверждения - быстрый повтор. Приём - по порядку
   в кольцо 16 КБ с окном по свободному месту; вне порядка - повтор подтверждения. Закрывший первым держит
   TIME_WAIT 2*MSL (60 с) в маленькой таблице (не в слоте соединения): на повтор FIN собеседника - снова ACK, четвёрка
   адресов не используется заново. На сегмент без соединения - RST (RFC 793), чтобы собеседник не ждал зря. */
#define TCP_MAX 8
#define TCP_RX  16384
#define TCP_TX  16384
#define LISTEN_MAX 4
#define BACKLOG 4
enum { T_FREE, T_SYN_SENT, T_SYN_RCVD, T_EST, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_LAST_ACK, T_CLOSED };
enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };
typedef struct {
    int state, reset, fin_rcvd, fin_sent, accepted, listener;
    ipa_t rip, lip; uint16_t rport, lport; uint8_t mac[6];
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt; uint16_t mss;
    uint8_t tx[TCP_TX]; uint32_t tx_len;                      /* данные с snd_una (неподтверждённые + ждущие) */
    uint64_t rto_at; unsigned tries, dupacks;
    uint8_t rx[TCP_RX]; uint32_t rx_head, rx_tail;           /* занято: tail - head */
} tcb_t;
static tcb_t tcbs[TCP_MAX];
static struct { uint16_t port; int used; uint8_t fam; } listens[LISTEN_MAX];
static spinlock_t tlk = SPINLOCK_INIT;
static uint16_t next_lport = 49152;

#define TW_MAX 32
#define TW_TICKS (60 * TIMER_HZ)                                   /* 2*MSL: MSL = 30 с */
static struct { ipa_t rip, lip; uint32_t snd_nxt, rcv_nxt; uint16_t rport, lport; uint8_t mac[6]; uint64_t until; } tw[TW_MAX];
static uint64_t tw_total;
static int tw_find(const ipa_t* rip, uint16_t rport, uint16_t lport, uint64_t now) {
    for (int i = 0; i < TW_MAX; i++)
        if (tw[i].until > now && ipa_eq(&tw[i].rip, rip) && tw[i].rport == rport && tw[i].lport == lport) return i;
    return -1;
}

static uint16_t tcp_win(const tcb_t* t) { uint32_t f = TCP_RX - (t->rx_tail - t->rx_head); return (uint16_t)(f > 65535 ? 65535 : f); }

static uint16_t tcp_mss_max(uint8_t fam) { return fam == 4 ? 1460 : (uint16_t)(mtu6 - 60); }

static void tcp_seg(const uint8_t* mac, const ipa_t* lip, const ipa_t* rip, uint16_t lport, uint16_t rport, uint8_t flags, uint32_t seq,
                    uint32_t ack, uint16_t win, const uint8_t* data, uint32_t len) {
    uint8_t h[24 + 1460];
    uint32_t hl = (flags & F_SYN) ? 24 : 20;
    put16(h, lport); put16(h + 2, rport); put32(h + 4, seq); put32(h + 8, (flags & F_ACK) ? ack : 0);
    h[12] = (uint8_t)((hl / 4) << 4); h[13] = flags; put16(h + 14, win); put16(h + 16, 0); put16(h + 18, 0);
    if (flags & F_SYN) { h[20] = 2; h[21] = 4; put16(h + 22, tcp_mss_max(rip->fam)); }   /* MSS */
    cpy(h + hl, data, len);
    put16(h + 16, rip->fam == 4 ? net_csum4(ipa_to4(lip), ipa_to4(rip), 6, h, hl + len) : net_csum6(lip->a, rip->a, 6, h, hl + len));
    ipa_send(mac, lip, rip, 6, h, hl + len);
}
static void tcp_out(tcb_t* t, uint8_t flags, uint32_t seq, const uint8_t* data, uint32_t len) {
    tcp_seg(t->mac, &t->lip, &t->rip, t->lport, t->rport, flags, seq, t->rcv_nxt, tcp_win(t), data, len);
}

/* Отправить, что позволяет окно (под tlk). SYN и FIN занимают по номеру, данных у них нет. */
static void tcp_push(tcb_t* t, uint64_t now) {
    if (t->state == T_SYN_SENT || t->state == T_SYN_RCVD || t->state == T_CLOSED) return;
    uint32_t wnd = t->snd_wnd ? t->snd_wnd : t->mss;
    for (;;) {
        uint32_t flight = t->snd_nxt - t->snd_una - (t->fin_sent && t->snd_nxt != t->snd_una ? 1 : 0);
        if (flight >= t->tx_len || flight >= wnd) break;
        uint32_t n = t->tx_len - flight;
        if (n > t->mss) n = t->mss;
        if (n > wnd - flight) n = wnd - flight;
        if (!n) break;
        uint8_t seg[1460];
        for (uint32_t i = 0; i < n; i++) seg[i] = t->tx[(flight + i) % TCP_TX];
        tcp_out(t, F_ACK | F_PSH, t->snd_una + flight, seg, n);
        t->snd_nxt = t->snd_una + flight + n;
        if (!t->rto_at) t->rto_at = now + TIMER_HZ;
    }
}

/* Повтор первого неподтверждённого (по сроку или трём повторам подтверждения). */
static void tcp_retransmit(tcb_t* t) {
    if (t->state == T_SYN_SENT) { tcp_out(t, F_SYN, t->iss, 0, 0); return; }
    if (t->state == T_SYN_RCVD) { tcp_out(t, F_SYN | F_ACK, t->iss, 0, 0); return; }
    uint32_t n = t->tx_len < t->mss ? t->tx_len : t->mss;
    if (n) {
        uint8_t seg[1460];
        for (uint32_t i = 0; i < n; i++) seg[i] = t->tx[i % TCP_TX];
        tcp_out(t, F_ACK | F_PSH, t->snd_una, seg, n);
    } else if (t->fin_sent) tcp_out(t, F_FIN | F_ACK, t->snd_nxt - 1, 0, 0);
}

static tcb_t* tcb_new(void) {
    for (int i = 0; i < TCP_MAX; i++) if (tcbs[i].state == T_FREE) {
        tcb_t* t = &tcbs[i];
        t->reset = t->fin_rcvd = t->fin_sent = t->accepted = 0; t->listener = -1;
        t->rx_head = t->rx_tail = 0; t->tx_len = 0; t->rto_at = 0; t->tries = 0; t->dupacks = 0;
        t->mss = 536; t->snd_wnd = 0;
        return t;
    }
    return 0;
}

static void tcp_input(const ipa_t* src, const ipa_t* dst, const uint8_t* eth_src, const uint8_t* g, uint32_t len) {
    if (len < 20) return;
    uint32_t hl = (uint32_t)(g[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    if ((src->fam == 4 ? net_csum4(ipa_to4(src), ipa_to4(dst), 6, g, len) : net_csum6(src->a, dst->a, 6, g, len)) != 0) return;   /* сумма с псевдозаголовком */
    uint16_t sport = get16(g), dport = get16(g + 2);
    uint32_t seq = get32(g + 4), ack = get32(g + 8);
    uint8_t fl = g[13];
    uint16_t win = get16(g + 14);
    const uint8_t* data = g + hl; uint32_t dl = len - hl;
    uint16_t mss = src->fam == 4 ? 536 : 1220;                  /* без опции MSS: 536 (IPv4) и 1220 (IPv6) */
    for (uint32_t i = 20; i + 1 < hl; ) {                       /* MSS собеседника (в SYN) */
        uint8_t k = g[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (g[i + 1] < 2) break;
        if (k == 2 && g[i + 1] == 4 && i + 4 <= hl) mss = get16(g + i + 2);
        i += g[i + 1];
    }
    if (mss > tcp_mss_max(src->fam)) mss = tcp_mss_max(src->fam);
    uint64_t now = timer_get_ticks();
    uint64_t f = spin_lock_irqsave(&tlk);
    tcb_t* t = 0;
    for (int i = 0; i < TCP_MAX; i++)
        if (tcbs[i].state != T_FREE && ipa_eq(&tcbs[i].rip, src) && tcbs[i].rport == sport && tcbs[i].lport == dport && ipa_eq(&tcbs[i].lip, dst)) t = &tcbs[i];
    if (!t) {
        int w = tw_find(src, sport, dport, now);
        if (w >= 0) {                                           /* TIME_WAIT: повтор FIN - снова подтвердить */
            if (fl & F_FIN) tcp_seg(tw[w].mac, &tw[w].lip, src, dport, sport, F_ACK, tw[w].snd_nxt, tw[w].rcv_nxt, 0, 0, 0);
            spin_unlock_irqrestore(&tlk, f);
            return;
        }
    }
    if (!t) {                                                   /* новое входящее: только SYN на слушаемый порт */
        int l = -1;
        for (int i = 0; i < LISTEN_MAX; i++) if (listens[i].used && listens[i].port == dport && listens[i].fam == src->fam) l = i;
        if (l >= 0 && (fl & (F_SYN | F_ACK | F_RST)) == F_SYN) {
            unsigned pending = 0;
            for (int i = 0; i < TCP_MAX; i++) if (tcbs[i].state != T_FREE && tcbs[i].listener == l && !tcbs[i].accepted) pending++;
            tcb_t* n = pending < BACKLOG ? tcb_new() : 0;
            if (n) {
                n->rip = *src; n->lip = *dst; n->rport = sport; n->lport = dport; cpy(n->mac, eth_src, 6); n->listener = l;
                n->iss = (uint32_t)(clock_ns() >> 6); n->snd_una = n->iss; n->snd_nxt = n->iss + 1;
                n->rcv_nxt = seq + 1; n->mss = mss; n->snd_wnd = win;
                n->state = T_SYN_RCVD; n->rto_at = now + TIMER_HZ;
                tcp_out(n, F_SYN | F_ACK, n->iss, 0, 0);
            }
        } else if (l < 0 && !(fl & F_RST) && (src->fam == 6 || my_ip)) {   /* порт закрыт: RST, а не молчание */
            if (fl & F_ACK) tcp_seg(eth_src, dst, src, dport, sport, F_RST, ack, 0, 0, 0, 0);
            else tcp_seg(eth_src, dst, src, dport, sport, F_RST | F_ACK, 0, seq + dl + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0), 0, 0, 0);
        }
        spin_unlock_irqrestore(&tlk, f);
        return;
    }
    if (t->state == T_CLOSED) { spin_unlock_irqrestore(&tlk, f); return; }
    if (fl & F_RST) { t->reset = 1; t->state = T_CLOSED; spin_unlock_irqrestore(&tlk, f); return; }
    if (t->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1; t->snd_una = ack; t->mss = mss; t->snd_wnd = win; t->rto_at = 0; t->tries = 0;
            t->state = T_EST;
            tcp_out(t, F_ACK, t->snd_nxt, 0, 0);
        }
        spin_unlock_irqrestore(&tlk, f);
        return;
    }
    if (t->state == T_SYN_RCVD) {
        if ((fl & F_ACK) && ack == t->snd_nxt) { t->snd_una = ack; t->state = T_EST; t->rto_at = 0; t->tries = 0; t->snd_wnd = win; }
        else if (fl & F_SYN) { tcp_out(t, F_SYN | F_ACK, t->iss, 0, 0); spin_unlock_irqrestore(&tlk, f); return; }   /* повтор SYN */
        else { spin_unlock_irqrestore(&tlk, f); return; }
    }
    if (fl & F_ACK) {
        int32_t adv = (int32_t)(ack - t->snd_una);
        if (adv > 0 && (int32_t)(ack - t->snd_nxt) <= 0) {
            uint32_t data_acked = (uint32_t)adv;
            if (t->fin_sent && ack == t->snd_nxt) data_acked--;   /* номер FIN - не данные */
            if (data_acked > t->tx_len) data_acked = t->tx_len;
            for (uint32_t i = 0; i + data_acked < t->tx_len; i++) t->tx[i] = t->tx[i + data_acked];
            t->tx_len -= data_acked;
            t->snd_una = ack; t->tries = 0; t->dupacks = 0;
            t->rto_at = t->snd_una != t->snd_nxt ? now + TIMER_HZ : 0;
            if (ack == t->snd_nxt && t->fin_sent) {
                if (t->state == T_FIN_WAIT1) t->state = T_FIN_WAIT2;
                else if (t->state == T_LAST_ACK) t->state = T_CLOSED;
            }
        } else if (adv == 0 && !dl && t->snd_una != t->snd_nxt && ++t->dupacks == 3) tcp_retransmit(t);   /* быстрый повтор */
        t->snd_wnd = win;
    }
    int need_ack = 0;
    if (dl) {
        if (seq == t->rcv_nxt) {
            uint32_t room = TCP_RX - (t->rx_tail - t->rx_head), take = dl < room ? dl : room;
            for (uint32_t i = 0; i < take; i++) t->rx[(t->rx_tail + i) % TCP_RX] = data[i];
            t->rx_tail += take; t->rcv_nxt += take;
        }
        need_ack = 1;                                               /* и на чужой порядок - повтор подтверждения */
    }
    if ((fl & F_FIN) && seq + dl == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++; t->fin_rcvd = 1; need_ack = 1;
        if (t->state == T_EST) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1 || t->state == T_FIN_WAIT2) {   /* закрыли первыми: TIME_WAIT */
            int k = 0;
            for (int i = 1; i < TW_MAX; i++) if (tw[i].until < tw[k].until) k = i;   /* свободная или самая старая */
            tw[k].rip = t->rip; tw[k].lip = t->lip; tw[k].rport = t->rport; tw[k].lport = t->lport; cpy(tw[k].mac, t->mac, 6);
            tw[k].snd_nxt = t->snd_nxt; tw[k].rcv_nxt = t->rcv_nxt; tw[k].until = now + TW_TICKS;
            tw_total++;
            t->state = T_CLOSED;
        }
    }
    tcp_push(t, now);
    if (need_ack) tcp_out(t, F_ACK, t->snd_nxt, 0, 0);
    spin_unlock_irqrestore(&tlk, f);
}

/* Сроки повтора (поток «net», каждый тик). */
static void tcp_tick(uint64_t now) {
    uint64_t f = spin_lock_irqsave(&tlk);
    for (int i = 0; i < TCP_MAX; i++) {
        tcb_t* t = &tcbs[i];
        if (t->state == T_FREE || t->state == T_CLOSED || !t->rto_at || now < t->rto_at) continue;
        if (++t->tries > 8) { t->state = T_CLOSED; t->reset = 1; t->rto_at = 0; continue; }
        t->rto_at = now + ((uint64_t)TIMER_HZ << (t->tries < 6 ? t->tries : 6));
        tcp_retransmit(t);
    }
    spin_unlock_irqrestore(&tlk, f);
}

static int tcp_wait(tcb_t* t, int (*cond)(const tcb_t*), uint64_t ticks) {
    uint64_t until = timer_get_ticks() + ticks;
    while (!cond(t)) {
        if (timer_get_ticks() >= until) return -1;
        sched_sleep_ticks(1);
    }
    return 0;
}
static int c_open(const tcb_t* t) { return t->state != T_SYN_SENT; }
static int c_room(const tcb_t* t) { return t->tx_len < TCP_TX || t->state == T_CLOSED; }
static int c_sent(const tcb_t* t) { return t->tx_len == 0 || t->state == T_CLOSED; }
static int c_data(const tcb_t* t) { return t->rx_tail != t->rx_head || t->fin_rcvd || t->state == T_CLOSED; }
static int c_closed(const tcb_t* t) { return t->state == T_CLOSED; }

static int tcp_connect_ipa(const ipa_t* dst, uint16_t port) {
    uint8_t mac[6];
    ipa_t lip;
    if (!dev) return -1;
    if (dst->fam == 4) {
        if (dstate != D_BOUND) return -1;
        lip = ipa_v4(my_ip);
        if (resolve(ipa_to4(dst), mac) != 0) return -2;
    } else {
        uint8_t s[16];
        if (!v6_on || src6_for(dst->a, s) != 0) return -1;
        lip = ipa_v6(s);
        if (resolve6(dst->a, mac) != 0) return -2;
    }
    uint64_t f = spin_lock_irqsave(&tlk);
    tcb_t* t = tcb_new();
    if (!t) { spin_unlock_irqrestore(&tlk, f); return -3; }
    uint64_t now0 = timer_get_ticks();
    do {                                                            /* четвёрка в TIME_WAIT заново не берётся */
        t->lport = next_lport++;
        if (next_lport < 49152) next_lport = 49152;
    } while (tw_find(dst, port, t->lport, now0) >= 0);
    t->rip = *dst; t->lip = lip; t->rport = port;
    cpy(t->mac, mac, 6);
    t->iss = (uint32_t)(clock_ns() >> 6); t->snd_una = t->iss; t->snd_nxt = t->iss + 1; t->rcv_nxt = 0;
    t->state = T_SYN_SENT; t->rto_at = timer_get_ticks() + TIMER_HZ;
    tcp_out(t, F_SYN, t->iss, 0, 0);
    spin_unlock_irqrestore(&tlk, f);
    if (tcp_wait(t, c_open, 30 * TIMER_HZ) != 0 || t->state != T_EST) { t->state = T_FREE; return -4; }
    return (int)(t - tcbs);
}
int tcp_connect(uint32_t ip, uint16_t port) { ipa_t d = ipa_v4(ip); return tcp_connect_ipa(&d, port); }
int tcp_connect6(const uint8_t ip[16], uint16_t port) { ipa_t d = ipa_v6(ip); return tcp_connect_ipa(&d, port); }

static int tcp_listen_fam(uint16_t port, uint8_t fam) {
    uint64_t f = spin_lock_irqsave(&tlk);
    int l = -1;
    for (int i = 0; i < LISTEN_MAX; i++) if (listens[i].used && listens[i].port == port && listens[i].fam == fam) { spin_unlock_irqrestore(&tlk, f); return -3; }
    for (int i = 0; i < LISTEN_MAX && l < 0; i++) if (!listens[i].used) l = i;
    if (l >= 0) { listens[l].used = 1; listens[l].port = port; listens[l].fam = fam; }
    spin_unlock_irqrestore(&tlk, f);
    return l < 0 ? -9 : l;
}
int tcp_listen(uint16_t port) { return tcp_listen_fam(port, 4); }
int tcp_listen6(uint16_t port) { return tcp_listen_fam(port, 6); }
/* Адрес собеседника соединения: семейство (4/6) или <0. */
int tcp_peer(int h, uint8_t addr[16], uint16_t* port) {
    if (h < 0 || h >= TCP_MAX || tcbs[h].state == T_FREE) return -1;
    cpy(addr, tcbs[h].rip.a, 16); *port = tcbs[h].rport;
    return tcbs[h].rip.fam;
}

int tcp_accept(int l, uint32_t timeout_ticks) {
    if (l < 0 || l >= LISTEN_MAX || !listens[l].used) return -1;
    uint64_t until = timer_get_ticks() + timeout_ticks;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&tlk);
        for (int i = 0; i < TCP_MAX; i++) {
            tcb_t* t = &tcbs[i];
            if (t->listener == l && !t->accepted && t->state != T_FREE && t->state != T_SYN_RCVD) {
                t->accepted = 1;
                spin_unlock_irqrestore(&tlk, f);
                return i;
            }
        }
        spin_unlock_irqrestore(&tlk, f);
        if (timer_get_ticks() >= until) return -5;
        sched_sleep_ticks(1);
    }
}

void tcp_unlisten(int l) {
    if (l < 0 || l >= LISTEN_MAX) return;
    uint64_t f = spin_lock_irqsave(&tlk);
    listens[l].used = 0;
    for (int i = 0; i < TCP_MAX; i++)                            /* не принятые - сбросить */
        if (tcbs[i].listener == l && !tcbs[i].accepted && tcbs[i].state != T_FREE) { tcp_out(&tcbs[i], F_RST | F_ACK, tcbs[i].snd_nxt, 0, 0); tcbs[i].state = T_FREE; }
    spin_unlock_irqrestore(&tlk, f);
}

int tcp_send(int h, const void* data, uint32_t len) {
    if (h < 0 || h >= TCP_MAX) return -1;
    tcb_t* t = &tcbs[h];
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0;
    while (done < len) {
        if (t->state != T_EST && t->state != T_CLOSE_WAIT) return done ? (int)done : -1;
        if (tcp_wait(t, c_room, 60 * TIMER_HZ) != 0 || t->state == T_CLOSED) return done ? (int)done : -2;
        uint64_t f = spin_lock_irqsave(&tlk);
        uint32_t room = TCP_TX - t->tx_len, n = len - done < room ? len - done : room;
        for (uint32_t i = 0; i < n; i++) t->tx[t->tx_len + i] = p[done + i];
        t->tx_len += n;
        tcp_push(t, timer_get_ticks());
        spin_unlock_irqrestore(&tlk, f);
        done += n;
    }
    if (tcp_wait(t, c_sent, 60 * TIMER_HZ) != 0 || t->reset) return -2;    /* вызов возвращается, когда всё подтверждено */
    return (int)done;
}

int tcp_recv(int h, void* buf, uint32_t max, uint32_t timeout_ticks) {
    if (h < 0 || h >= TCP_MAX) return -1;
    tcb_t* t = &tcbs[h];
    if (tcp_wait(t, c_data, timeout_ticks) != 0) return -1;
    uint64_t f = spin_lock_irqsave(&tlk);
    uint32_t have = t->rx_tail - t->rx_head, n = have < max ? have : max;
    uint32_t before = TCP_RX - have;
    uint8_t* o = (uint8_t*)buf;
    for (uint32_t i = 0; i < n; i++) o[i] = t->rx[(t->rx_head + i) % TCP_RX];
    t->rx_head += n;
    if (n && before < t->mss && TCP_RX - (t->rx_tail - t->rx_head) >= t->mss && t->state != T_CLOSED)
        tcp_out(t, F_ACK, t->snd_nxt, 0, 0);                         /* окно снова открыто - сообщить */
    int closed = !n && (t->fin_rcvd || t->state == T_CLOSED);
    int reset = t->reset;
    spin_unlock_irqrestore(&tlk, f);
    if (closed) return reset ? -2 : 0;
    return (int)n;
}

void tcp_close(int h) {
    if (h < 0 || h >= TCP_MAX) return;
    tcb_t* t = &tcbs[h];
    tcp_wait(t, c_sent, 10 * TIMER_HZ);                          /* сначала - отправленное */
    uint64_t f = spin_lock_irqsave(&tlk);
    if (t->state == T_EST || t->state == T_CLOSE_WAIT) {
        t->state = t->state == T_EST ? T_FIN_WAIT1 : T_LAST_ACK;
        t->fin_sent = 1;
        tcp_out(t, F_FIN | F_ACK, t->snd_nxt, 0, 0);
        t->snd_nxt++;
        t->rto_at = timer_get_ticks() + TIMER_HZ;
    }
    spin_unlock_irqrestore(&tlk, f);
    tcp_wait(t, c_closed, 3 * TIMER_HZ);
    f = spin_lock_irqsave(&tlk);
    t->state = T_FREE; t->rto_at = 0; t->listener = -1;
    spin_unlock_irqrestore(&tlk, f);
}


/* ---------- сеть для программ (системные вызовы) ----------
   Соединение принадлежит процессу, который его открыл: чужой номер - отказ; при завершении процесса его соединения
   закрываются (net_proc_gone). Данные копируются через буфер ядра проверенными копиями (uaccess). */
#include "uaccess.h"
#include "proc.h"
static uint32_t tcp_owner[TCP_MAX], listen_owner[LISTEN_MAX];
static int own(int64_t h) { return h >= 0 && h < TCP_MAX && tcp_owner[h] && tcp_owner[h] == proc_current_pid(); }
#define LH 100                                                   /* номера слушателей: 100 + индекс */
static int own_l(int64_t h) { return h >= LH && h < LH + LISTEN_MAX && listen_owner[h - LH] == proc_current_pid(); }

int64_t net_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (op == NET_SYS_DNS) {                                     /* (имя, длина) -> адрес */
        char name[128];
        if (!a2 || a2 >= sizeof name || copy_from_user(name, a1, a2)) return -10;
        name[a2] = 0;
        uint32_t ip;
        int r = net_dns(name, &ip);
        return r ? r : (int64_t)ip;
    }
    if (op == NET_SYS_CONNECT) {                                 /* (адрес, порт) -> номер соединения */
        if (a1 > 0xFFFFFFFFull || !a2 || a2 > 65535) return -10;
        int h = tcp_connect((uint32_t)a1, (uint16_t)a2);
        if (h >= 0) tcp_owner[h] = proc_current_pid();
        return h;
    }
    if (op == NET_SYS_CONNECT_SA) {                              /* (адрес сокета) -> номер соединения */
        ksockaddr_t sa;
        if (copy_from_user(&sa, a1, sizeof sa) || !sa.port || (sa.family != 4 && sa.family != 6)) return -10;
        int h = sa.family == 4 ? tcp_connect(get32(sa.addr), sa.port) : tcp_connect6(sa.addr, sa.port);
        if (h >= 0) tcp_owner[h] = proc_current_pid();
        return h;
    }
    if (op == NET_SYS_LISTEN) {                                  /* (порт | семейство << 16; 0 или 4 - IPv4, 6 - IPv6) -> номер слушателя */
        uint64_t fam = a1 >> 16;
        a1 &= 0xFFFF;
        if (!a1 || (fam && fam != 4 && fam != 6)) return -10;
        int l = fam == 6 ? tcp_listen6((uint16_t)a1) : tcp_listen((uint16_t)a1);
        if (l < 0) return l;
        listen_owner[l] = proc_current_pid();
        return LH + l;
    }
    if (op == NET_SYS_ACCEPT) {                                  /* (слушатель, мс) -> номер соединения */
        if (!own_l((int64_t)a1)) return -8;
        uint64_t ticks = a2 ? (a2 * TIMER_HZ + 999) / 1000 : ~0u;
        int h = tcp_accept((int)(a1 - LH), (uint32_t)(ticks > 0xFFFFFFFFull ? 0xFFFFFFFFull : ticks));
        if (h >= 0) tcp_owner[h] = proc_current_pid();
        return h;
    }
    if (op == NET_SYS_CLOSE && own_l((int64_t)a1)) { listen_owner[a1 - LH] = 0; tcp_unlisten((int)(a1 - LH)); return 0; }
    if (!own((int64_t)a1)) return -8;                            /* неверный дескриптор */
    int h = (int)a1;
    if (op == NET_SYS_SEND) {                                    /* (h, буфер, длина) -> отправлено */
        uint8_t b[1024]; uint64_t done = 0;
        if (a3 > (1u << 20) || (a3 && !user_range(a2, a3, 0))) return -10;
        while (done < a3) {
            uint64_t n = a3 - done < sizeof b ? a3 - done : sizeof b;
            if (copy_from_user(b, a2 + done, n)) return -10;
            int r = tcp_send(h, b, (uint32_t)n);
            if (r < 0) return done ? (int64_t)done : r;
            done += (uint64_t)r;
        }
        return (int64_t)done;
    }
    if (op == NET_SYS_RECV) {                                    /* (h, буфер, длина | мс << 32) -> байт, 0 - закрыто */
        uint8_t b[1024];
        uint64_t len = a3 & 0xFFFFFFFFull, ms = a3 >> 32;
        if (!len || !user_range(a2, len, 1)) return -10;
        if (len > sizeof b) len = sizeof b;
        uint64_t ticks = ms ? (ms * TIMER_HZ + 999) / 1000 : 10 * TIMER_HZ;
        int r = tcp_recv(h, b, (uint32_t)len, (uint32_t)ticks);
        if (r > 0 && copy_to_user(a2, b, (uint64_t)r)) return -10;
        return r;
    }
    if (op == NET_SYS_PEER) {                                    /* (h, адрес сокета) -> семейство собеседника */
        ksockaddr_t sa;
        for (unsigned i = 0; i < sizeof sa; i++) ((uint8_t*)&sa)[i] = 0;
        int fam = tcp_peer(h, sa.addr, &sa.port);
        if (fam < 0) return -8;
        sa.family = (uint16_t)fam;
        return copy_to_user(a2, &sa, sizeof sa) ? -10 : fam;
    }
    if (op == NET_SYS_CLOSE) { tcp_owner[h] = 0; tcp_close(h); return 0; }
    return -10;
}

static void udp_gone(uint32_t pid);
void net_proc_gone(uint32_t pid) {
    udp_gone(pid);
    for (int i = 0; i < LISTEN_MAX; i++) if (listen_owner[i] == pid) { listen_owner[i] = 0; tcp_unlisten(i); }
    for (int i = 0; i < TCP_MAX; i++) if (tcp_owner[i] == pid) { tcp_owner[i] = 0; tcp_close(i); }
}

/* ---------- приём ---------- */
static ipfrag_t frag;                                               /* сборка фрагментов IPv4 и IPv6 (только поток «net») */

static void icmp_input(const uint8_t* eth_src, uint32_t src, const uint8_t* m, uint32_t len) {
    if (len < 8 || net_csum(m, len) != 0) return;
    if (m[0] == 8 && my_ip) {                                       /* эхо-запрос: ответить тем же (большой - фрагментами) */
        static uint8_t r[IPF_MAX];
        if (len > sizeof r) return;
        cpy(r, m, len); r[0] = 0; put16(r + 2, 0); put16(r + 2, net_csum(r, len));
        ip_send(eth_src, my_ip, src, 1, r, len);
    } else if (m[0] == 0 && get16(m + 4) == 0x4B49 && get16(m + 6) == ping_seq && src == ping_ip) {
        ping_rx_ns = clock_ns();
        __atomic_store_n(&ping_got, 1, __ATOMIC_RELEASE);
    }
}

/* ---------- UDP для программ ----------
   Сокет - локальный порт, семейство (IPv4 или IPv6) и кольцо 16 КБ с записями «отправитель + данные» (до 8184 байт данных:
   столько собирается из фрагментов); принадлежит открывшему процессу. Приём - поток «net» кладёт в кольцо (сумма
   проверяется; в IPv6 она обязательна), нет места - датаграмма отбрасывается (счётчик). Порты 67/68 (DHCP ядра) и
   уже занятые - отказ. */
#define UDP_MAX 8
#define UDP_RING 16384
#define UDP_REC 22                                                  /* длина 2, порт 2, семейство 1, запас 1, адрес 16 */
#define UDP_DATA (IPF_MAX - 8)
#define UH 300
static struct { uint32_t owner; uint16_t lport; uint8_t fam; uint32_t head, tail; uint64_t dropped; uint8_t ring[UDP_RING]; } us[UDP_MAX];
static spinlock_t ulk = SPINLOCK_INIT;
static uint16_t next_uport = 50000;

static void ring_put(unsigned s, uint32_t pos, const uint8_t* d, uint32_t n) { for (uint32_t i = 0; i < n; i++) us[s].ring[(pos + i) % UDP_RING] = d[i]; }
static void ring_get(unsigned s, uint32_t pos, uint8_t* d, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[i] = us[s].ring[(pos + i) % UDP_RING]; }

static void udp_deliver(const ipa_t* src, uint16_t sport, uint16_t dport, const uint8_t* data, uint32_t n) {
    uint8_t h[UDP_REC];
    put16(h, (uint16_t)n); put16(h + 2, sport); h[4] = src->fam; h[5] = 0; cpy(h + 6, src->a, 16);
    uint64_t f = spin_lock_irqsave(&ulk);
    for (unsigned i = 0; i < UDP_MAX; i++) {
        if (!us[i].owner || us[i].lport != dport || us[i].fam != src->fam) continue;
        if (n > UDP_DATA || UDP_RING - (us[i].tail - us[i].head) < UDP_REC + n) { us[i].dropped++; break; }
        ring_put(i, us[i].tail, h, UDP_REC); ring_put(i, us[i].tail + UDP_REC, data, n);
        us[i].tail += UDP_REC + n;
        break;
    }
    spin_unlock_irqrestore(&ulk, f);
}

static void udp_input(const ipa_t* src, const ipa_t* dst, const uint8_t* d, uint32_t n) {
    uint16_t sport = get16(d), dport = get16(d + 2), ul = get16(d + 4);
    if (ul < 8 || ul > n) return;
    if (src->fam == 6 || get16(d + 6)) {                            /* IPv6: сумма обязательна; IPv4: если задана */
        uint16_t c = src->fam == 4 ? net_csum4(ipa_to4(src), ipa_to4(dst), 17, d, ul) : net_csum6(src->a, dst->a, 17, d, ul);
        if (c != 0) return;
        if (src->fam == 6 && !get16(d + 6)) return;
    }
    udp_deliver(src, sport, dport, d + 8, ul - 8);
}

/* Транспорт IPv4 после сборки: d - данные датаграммы. */
static void ip_deliver(const uint8_t* eth_src, uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* d, uint32_t n) {
    if (proto == 1) icmp_input(eth_src, src, d, n);
    else if (proto == 17 && n >= 8 && get16(d + 2) == 68 && get16(d) == 67) dhcp_input(d + 8, n - 8);
    else if (proto == 17 && n >= 8 && dns_port && get16(d + 2) == dns_port && get16(d) == 53 && src == my_dns) dns_input(d + 8, n - 8);
    else if (proto == 6) { ipa_t s = ipa_v4(src), t = ipa_v4(dst); tcp_input(&s, &t, eth_src, d, n); }
    else if (proto == 17 && n >= 8 && my_ip) { ipa_t s = ipa_v4(src), t = ipa_v4(dst); udp_input(&s, &t, d, n); }
}

static void ip_input(const uint8_t* eth_src, const uint8_t* p, uint32_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(p[0] & 15) * 4, tot = get16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || net_csum(p, ihl) != 0) return;
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    int to_us = (my_ip && dst == my_ip) || dst == 0xFFFFFFFF || (my_mask && dst == (my_ip | ~my_mask)) || (!my_ip && p[9] == 17);
    if (!to_us) return;
    const uint8_t* d = p + ihl; uint32_t n = tot - ihl;
    if (src && (!my_mask || (src & my_mask) == (my_ip & my_mask))) arp_learn(src, eth_src);
    uint16_t fo = get16(p + 6);
    if (fo & 0x3FFF) {                                              /* фрагмент: собрать (RFC 791/815) */
        ipf_key_t k;
        for (int i = 0; i < 16; i++) k.src[i] = k.dst[i] = 0;
        k.fam = 4; put32(k.src, src); put32(k.dst, dst); k.id = get16(p + 4); k.proto = p[9];
        ipf_out_t o;
        if (ipfrag_add(&frag, timer_get_ticks(), &k, (uint32_t)(fo & 0x1FFF) * 8, (fo & 0x2000) != 0, d, n, 0, &o) != 1) return;
        d = o.data; n = o.len;
    }
    ip_deliver(eth_src, src, dst, p[9], d, n);
}

/* Транспорт IPv6 после сборки. ds: состояние нашего адреса получателя (A6_*), 0 - получатель групповой. */
static void ip6_deliver(const uint8_t* eth_src, const uint8_t* src, const uint8_t* dst, uint8_t hop, int ds, uint8_t proto, const uint8_t* d, uint32_t n) {
    if (proto == 58) { icmp6_input(eth_src, src, dst, hop, ds, d, n); return; }
    if (ds != A6_PREF) return;                                      /* TCP и UDP - только на рабочий адрес */
    ipa_t s = ipa_v6(src), t = ipa_v6(dst);
    if (proto == 6) tcp_input(&s, &t, eth_src, d, n);
    else if (proto == 17 && n >= 8) udp_input(&s, &t, d, n);
}

static void ip6_input(const uint8_t* eth_src, const uint8_t* p, uint32_t len) {
    if (len < 40 || (p[0] >> 4) != 6 || !v6_on) return;
    uint32_t pl = get16(p + 4);
    if (!pl || pl + 40 > len) { v6_drop++; return; }                // «джамбограммы» не поддерживаются
    const uint8_t* src = p + 8; const uint8_t* dst = p + 24;
    if (ip6_is_mcast(src)) { v6_drop++; return; }
    int ds = 0;
    if (!ip6_is_mcast(dst)) { ds = a6_state(dst); if (ds == A6_FREE) return; }
    else if (!ip6_eq(dst, ALL_NODES) && !is_our_solicited(dst)) return;
    uint8_t proto; uint32_t off;
    int w = ip6_walk(p + 40, pl, p[6], &proto, &off);
    if (w < 0) { v6_drop++; return; }
    const uint8_t* d = p + 40 + off; uint32_t n = pl - off;
    if (w == 1) {                                                   /* заголовок фрагмента */
        const uint8_t* fh = d;
        uint16_t fo = get16(fh + 2);
        ipf_key_t k;
        k.fam = 6; cpy(k.src, src, 16); cpy(k.dst, dst, 16); k.id = get32(fh + 4); k.proto = 44;
        ipf_out_t o;
        if (ipfrag_add(&frag, timer_get_ticks(), &k, (uint32_t)(fo & 0xFFF8), (fo & 1) != 0, fh + 8, n - 8, fh[0], &o) != 1) return;
        if (ip6_walk(o.data, o.len, (uint8_t)o.aux, &proto, &off) != 0) { v6_drop++; return; }   /* во втором проходе фрагмента быть не может */
        d = o.data + off; n = o.len - off;
    }
    ip6_deliver(eth_src, src, dst, p[7], ds, proto, d, n);
}

static void udp_gone(uint32_t pid) { for (int i = 0; i < UDP_MAX; i++) if (us[i].owner == pid) us[i].owner = 0; }
static int uown(int64_t h) { return h >= UH && h < UH + UDP_MAX && us[h - UH].owner && us[h - UH].owner == proc_current_pid(); }

/* Отправка датаграммы из сокета s: адрес получателя dst, порт, данные из ядра. */
static int64_t udp_xmit(int s, const ipa_t* dst, uint16_t port, uint8_t* b, uint32_t n) {
    uint8_t mac[6]; ipa_t lip;
    uint16_t lport = us[s].lport;
    put16(b, lport); put16(b + 2, port); put16(b + 4, (uint16_t)(8 + n)); put16(b + 6, 0);
    if (dst->fam == 4) {
        if (resolve(ipa_to4(dst), mac) != 0) return -2;
        lip = ipa_v4(my_ip);
        uint16_t cs = net_csum4(my_ip, ipa_to4(dst), 17, b, 8 + n); put16(b + 6, cs ? cs : 0xFFFF);
    } else {
        uint8_t sa[16];
        if (8 + n + 40 > mtu6) return -12;                          /* IPv6 отправляет без разбиения: данные <= MTU - 48 */
        if (src6_for(dst->a, sa) != 0) return -1;
        if (resolve6(dst->a, mac) != 0) return -2;
        lip = ipa_v6(sa);
        uint16_t cs = net_csum6(lip.a, dst->a, 17, b, 8 + n); put16(b + 6, cs ? cs : 0xFFFF);   /* 0 - «нет суммы» в IPv6 нельзя */
    }
    ipa_send(mac, &lip, dst, 17, b, 8 + n);
    return (int64_t)n;
}

int64_t net_udp_sys(unsigned op, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (op == NET_UDP_OPEN) {                                       /* (порт или 0 | семейство << 16) -> сокет */
        uint64_t fam = a1 >> 16;
        a1 &= 0xFFFF;
        if (a1 == 67 || a1 == 68 || (fam && fam != 4 && fam != 6)) return -10;
        if (!dev || (fam == 6 ? !v6_on : dstate != D_BOUND)) return -1;
        uint64_t f = spin_lock_irqsave(&ulk);
        uint16_t port = (uint16_t)a1;
        uint8_t sf = fam == 6 ? 6 : 4;
        for (int tries = 0; !port && tries < 16000; tries++) {      /* свободный эфемерный */
            uint16_t c = next_uport++; if (next_uport < 50000) next_uport = 50000;
            int busy = 0; for (int i = 0; i < UDP_MAX; i++) if (us[i].owner && us[i].lport == c && us[i].fam == sf) busy = 1;
            if (!busy) port = c;
        }
        int slot = -1;
        for (int i = 0; i < UDP_MAX; i++) { if (us[i].owner && us[i].lport == port && us[i].fam == sf) { slot = -2; break; } if (!us[i].owner && slot == -1) slot = i; }
        if (slot >= 0) { us[slot].owner = proc_current_pid(); us[slot].lport = port; us[slot].fam = sf; us[slot].head = us[slot].tail = 0; us[slot].dropped = 0; }
        spin_unlock_irqrestore(&ulk, f);
        return slot >= 0 ? UH + slot : slot == -2 ? -3 : -4;
    }
    int64_t sock = (int64_t)(a1 & 0xFFFF);
    if (!uown(sock)) return -8;
    int s = (int)sock - UH;
    if (op == NET_UDP_CLOSE) { uint64_t f = spin_lock_irqsave(&ulk); us[s].owner = 0; spin_unlock_irqrestore(&ulk, f); return 0; }
    if (op == NET_UDP_SEND || op == NET_UDP_SENDTO) {
        /* SEND: (сокет | порт << 16 | адрес << 32, буфер, длина), сокет IPv4; SENDTO: (сокет | длина << 16, адрес сокета, буфер) */
        ipa_t dst; uint16_t port; uint64_t n; uint64_t buf;
        if (op == NET_UDP_SEND) {
            if (us[s].fam != 4) return -10;
            dst = ipa_v4((uint32_t)(a1 >> 32)); port = (uint16_t)(a1 >> 16); n = a3; buf = a2;
            if (!(uint32_t)(a1 >> 32)) return -10;
        } else {
            ksockaddr_t sa;
            if (copy_from_user(&sa, a2, sizeof sa) || sa.family != us[s].fam) return -10;
            if (sa.family == 4) dst = ipa_v4(get32(sa.addr)); else dst = ipa_v6(sa.addr);
            port = sa.port; n = a1 >> 16; buf = a3;
        }
        if (!port || n > UDP_DATA) return -10;
        uint8_t* b = (uint8_t*)kmalloc(8 + n);
        if (!b) return -7;
        int64_t r = -10;
        if (!n || !copy_from_user(b + 8, buf, n)) r = udp_xmit(s, &dst, port, b, (uint32_t)n);
        kfree(b);
        return r;
    }
    if (op == NET_UDP_RECV || op == NET_UDP_RECVFROM) {             /* (сокет, буфер, ёмкость | мс << 32) -> байт данных */
        uint32_t cap = (uint32_t)a3, ms = (uint32_t)(a3 >> 32);
        uint32_t hdr = op == NET_UDP_RECV ? 8 : 24;
        if (cap < hdr || !user_range(a2, cap, 1) || (op == NET_UDP_RECV && us[s].fam != 4)) return -10;
        uint64_t until = timer_get_ticks() + ((uint64_t)(ms ? ms : 10000) * TIMER_HZ + 999) / 1000;
        uint8_t* out = (uint8_t*)kmalloc(hdr + UDP_DATA);
        if (!out) return -7;
        for (;;) {
            uint64_t f = spin_lock_irqsave(&ulk);
            if (us[s].tail != us[s].head) {
                uint8_t h[UDP_REC];
                ring_get((unsigned)s, us[s].head, h, UDP_REC);
                uint32_t len = get16(h), n = len < cap - hdr ? len : cap - hdr;   /* не влезло - хвост теряется, как у recvfrom */
                if (op == NET_UDP_RECV) { cpy(out, h + 6, 4); put16(out + 4, get16(h + 2)); put16(out + 6, (uint16_t)len); }
                else {
                    ksockaddr_t sa;
                    sa.family = h[4]; sa.port = get16(h + 2); cpy(sa.addr, h + 6, 16);
                    cpy(out, &sa, sizeof sa); out[20] = (uint8_t)len; out[21] = (uint8_t)(len >> 8); out[22] = out[23] = 0;
                }
                ring_get((unsigned)s, us[s].head + UDP_REC, out + hdr, n);
                us[s].head += UDP_REC + len;
                spin_unlock_irqrestore(&ulk, f);
                int64_t r = copy_to_user(a2, out, hdr + n) ? -10 : (int64_t)n;
                kfree(out);
                return r;
            }
            spin_unlock_irqrestore(&ulk, f);
            if (timer_get_ticks() >= until) { kfree(out); return -5; }
            sched_sleep_ticks(1);
        }
    }
    return -10;
}

static void net_main(void* arg) {
    (void)arg;
    static uint8_t f[2048];
    int had_link = -1;
    ipfrag_init(&frag, 30 * TIMER_HZ, 2 * TIMER_HZ);              /* собираемое живёт 30 с; при нехватке мест вытесняется старше 2 с */
    uint32_t mark = dev->iw ? irqwait_mark(dev->iw) : 0;
    for (;;) {
        int n;
        while ((n = dev->recv(dev, f, sizeof f)) > 0) {
            rx_frames++;
            if (n < 14) { rx_drop++; continue; }
            uint16_t type = get16(f + 12);
            if (type == 0x0806) arp_input(f + 14, (uint32_t)n - 14);
            else if (type == 0x0800) ip_input(f + 6, f + 14, (uint32_t)n - 14);
            else if (type == 0x86DD) ip6_input(f + 6, f + 14, (uint32_t)n - 14);
        }
        int l = dev->link(dev);
        if (l != had_link) {
            if (had_link >= 0) uart_print(l ? "[KIRA NET] link up\n" : "[KIRA NET] link down\n");
            had_link = l;
            if (!l) { my_ip = 0; dstate = D_INIT; v6_down(); }
            else v6_up(timer_get_ticks());
        }
        dhcp_tick(timer_get_ticks());
        tcp_tick(timer_get_ticks());
        v6_tick(timer_get_ticks());
        ipfrag_expire(&frag, timer_get_ticks());
        if (dev->iw) { irqwait_wait(dev->iw, mark, 1); mark = irqwait_mark(dev->iw); }   /* до прерывания (не дольше тика) */
        else sched_sleep_ticks(1);
    }
}

void net_register(netdev_t* d) { if (!dev) dev = d; }

int net_start(void) {
    if (!dev) { uart_print("[KIRA NET] No supported network card: networking is off.\n"); return 0; }
    if (!sched_create("net", net_main, 0, PRIO_NORMAL)) { uart_print("[KIRA NET] cannot create network thread.\n"); return 0; }
    uart_print("[KIRA NET] Network stack online: Ethernet, ARP, IPv4 (fragments), ICMP, UDP, DHCP client, DNS, TCP, IPv6 (ND, SLAAC, ICMPv6, UDP, TCP) (commands: net, ping, ping6, dns, http).\n");
    return 1;
}

/* ---------- команды ---------- */
void netcmd_net(const char* a) {
    (void)a;
    if (!dev) { uart_print("сетевой карты нет (поддерживаются Intel e1000 и virtio-net: в Limbo - Network: User, NIC: e1000 или virtio)\n"); return; }
    uart_print("карта "); uart_print(dev->name); uart_print(", канал "); uart_print(dev->link(dev) ? "есть" : "нет");
    {
        uint64_t now = timer_get_ticks(); unsigned n = 0;
        for (int i = 0; i < TW_MAX; i++) if (tw[i].until > now) n++;
        uart_print(", TIME_WAIT: "); uart_print_dec(n); uart_print(" (всего "); uart_print_dec(tw_total); uart_print(")");
    }
    if (dev->iw) { uart_print(", прерываний приёма "); uart_print_dec(__atomic_load_n(&dev->iw->fired, __ATOMIC_RELAXED)); }
    else uart_print(", приём опросом");
    uart_print("\n");
    if (dstate == D_BOUND) {
        uart_print("адрес "); print_ip(my_ip); uart_print(", маска "); print_ip(my_mask); uart_print(", шлюз "); print_ip(my_gw);
        uart_print(", DNS "); print_ip(my_dns); uart_print(" (DHCP, аренда "); uart_print_dec(lease_s); uart_print(" с)\n");
    } else {
        uart_print("адрес ещё не получен: DHCP "); uart_print(dstate == D_SELECTING ? "ищет сервер" : dstate == D_REQUESTING ? "запрашивает адрес" : "ждёт канала");
        uart_print("\n");
    }
    uart_print("кадров принято "); uart_print_dec(rx_frames); uart_print(", отправлено "); uart_print_dec(tx_frames);
    uart_print(", отброшено "); uart_print_dec(rx_drop); uart_print("\n");
    uart_print("фрагменты IP: собрано "); uart_print_dec(frag.assembled); uart_print(", собирается "); uart_print_dec(ipfrag_pending(&frag));
    uart_print(", повторов "); uart_print_dec(frag.dup); uart_print(", перекрытий "); uart_print_dec(frag.overlap);
    uart_print(", негодных "); uart_print_dec(frag.malformed); uart_print(", слишком больших "); uart_print_dec(frag.toobig);
    uart_print(", не нашлось места "); uart_print_dec(frag.nospace); uart_print(", по сроку "); uart_print_dec(frag.expired); uart_print("\n");
    if (!v6_on) { uart_print(v6_dup ? "IPv6: выключен (адрес канала уже занят другим узлом)\n" : "IPv6: выключен (нет канала)\n"); return; }
    uint64_t f = spin_lock_irqsave(&lk6), now = timer_get_ticks();
    for (int i = 0; i < A6_MAX; i++) {
        if (a6[i].state == A6_FREE) continue;
        uart_print("IPv6: "); print_ip6(a6[i].a); uart_print("/"); uart_print_dec(a6[i].plen);
        uart_print(a6[i].state == A6_TENT ? " (проверка повтора)" : a6[i].ll ? " (адрес канала)" : " (SLAAC)"); uart_print("\n");
    }
    if (gw6_set && now < gw6_until) { uart_print("IPv6: маршрутизатор "); print_ip6(gw6); uart_print("\n"); }
    if (dns6_set) { uart_print("IPv6: DNS (RDNSS) "); print_ip6(dns6); uart_print("\n"); }
    unsigned nn = 0; for (int i = 0; i < ND_MAX; i++) nn += nd6[i].st == 2;
    uart_print("IPv6: соседей в кэше "); uart_print_dec(nn); uart_print(", MTU "); uart_print_dec(mtu6); uart_print(", отброшено ND/IP "); uart_print_dec(v6_drop); uart_print("\n");
    spin_unlock_irqrestore(&lk6, f);
}

void netcmd_ping(const char* a) {
    uint32_t ip;
    while (a && *a == ' ') a++;
    if (!a || net_parse_ip(a, &ip) != 0) { uart_print("ping адрес [число], например: ping 10.0.2.2\n"); return; }
    while (*a && *a != ' ') a++;
    while (*a == ' ') a++;
    unsigned count = 4;
    if (*a >= '0' && *a <= '9') { count = 0; while (*a >= '0' && *a <= '9') count = count * 10 + (unsigned)(*a++ - '0'); }
    if (count < 1) count = 1;
    if (count > 100) count = 100;
    if (!dev || dstate != D_BOUND) { uart_print("сети нет: адрес не получен (команда net)\n"); return; }
    uint8_t mac[6];
    if (resolve(ip, mac) != 0) { uart_print("узел "); print_ip(ip); uart_print(" не отвечает на ARP\n"); return; }
    unsigned ok = 0; uint64_t sum_us = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t m[8 + 32];
        uint16_t seq = (uint16_t)(ping_seq + 1);
        m[0] = 8; m[1] = 0; put16(m + 2, 0); put16(m + 4, 0x4B49); put16(m + 6, seq);
        for (int k = 0; k < 32; k++) m[8 + k] = (uint8_t)('a' + k % 26);
        put16(m + 2, net_csum(m, sizeof m));
        ping_ip = ip; ping_seq = seq;
        __atomic_store_n(&ping_got, 0, __ATOMIC_RELEASE);
        uint64_t t0 = clock_ns();
        ip_send(mac, my_ip, ip, 1, m, sizeof m);
        uint64_t until = timer_get_ticks() + TIMER_HZ;
        while (!__atomic_load_n(&ping_got, __ATOMIC_ACQUIRE) && timer_get_ticks() < until) sched_sleep_ticks(1);
        if (ping_got) {
            uint64_t us = (ping_rx_ns - t0) / 1000;
            ok++; sum_us += us;
            uart_print("ответ от "); print_ip(ip); uart_print(": seq "); uart_print_dec(seq);
            uart_print(", время "); uart_print_dec(us / 1000); uart_print(","); uart_print_dec(us / 100 % 10); uart_print(" мс\n");
            uint64_t next = timer_get_ticks() + TIMER_HZ / 2;
            while (i + 1 < count && timer_get_ticks() < next) sched_sleep_ticks(1);
        } else {
            uart_print("нет ответа от "); print_ip(ip); uart_print(" (seq "); uart_print_dec(seq); uart_print(")\n");
        }
    }
    uart_print("отправлено "); uart_print_dec(count); uart_print(", получено "); uart_print_dec(ok);
    if (ok) { uart_print(", среднее "); uart_print_dec(sum_us / ok / 1000); uart_print(","); uart_print_dec(sum_us / ok / 100 % 10); uart_print(" мс"); }
    uart_print("\n");
}

void netcmd_ping6(const char* a) {
    uint8_t ip[16], mac[6], src[16];
    while (a && *a == ' ') a++;
    if (!a || ip6_parse(a, ip) != 0) { uart_print("ping6 адрес [число], например: ping6 fec0::2\n"); return; }
    while (*a && *a != ' ') a++;
    while (*a == ' ') a++;
    unsigned count = 4;
    if (*a >= '0' && *a <= '9') { count = 0; while (*a >= '0' && *a <= '9') count = count * 10 + (unsigned)(*a++ - '0'); }
    if (count < 1) count = 1;
    if (count > 100) count = 100;
    if (!dev || !v6_on || src6_for(ip, src) != 0) { uart_print("IPv6 не готов: рабочего адреса нет (команда net)\n"); return; }
    if (resolve6(ip, mac) != 0) { uart_print("узел "); print_ip6(ip); uart_print(" не отвечает на Neighbor Solicitation\n"); return; }
    unsigned ok = 0; uint64_t sum_us = 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t m[8 + 32], d[32];
        uint16_t seq = (uint16_t)(ping6_seq + 1);
        for (int k = 0; k < 32; k++) d[k] = (uint8_t)('a' + k % 26);
        uint32_t n = icmp6_build_echo(m, src, ip, 128, 0x4B49, seq, d, 32);
        cpy(ping6_ip, ip, 16); ping6_seq = seq;
        __atomic_store_n(&ping6_got, 0, __ATOMIC_RELEASE);
        uint64_t t0 = clock_ns();
        ip6_send(mac, src, ip, 58, 64, m, n);
        uint64_t until = timer_get_ticks() + TIMER_HZ;
        while (!__atomic_load_n(&ping6_got, __ATOMIC_ACQUIRE) && timer_get_ticks() < until) sched_sleep_ticks(1);
        if (ping6_got) {
            uint64_t us = (ping6_rx_ns - t0) / 1000;
            ok++; sum_us += us;
            uart_print("ответ от "); print_ip6(ip); uart_print(": seq "); uart_print_dec(seq);
            uart_print(", время "); uart_print_dec(us / 1000); uart_print(","); uart_print_dec(us / 100 % 10); uart_print(" мс\n");
            uint64_t next = timer_get_ticks() + TIMER_HZ / 2;
            while (i + 1 < count && timer_get_ticks() < next) sched_sleep_ticks(1);
        } else {
            uart_print("нет ответа от "); print_ip6(ip); uart_print(" (seq "); uart_print_dec(seq); uart_print(")\n");
        }
    }
    uart_print("отправлено "); uart_print_dec(count); uart_print(", получено "); uart_print_dec(ok);
    if (ok) { uart_print(", среднее "); uart_print_dec(sum_us / ok / 1000); uart_print(","); uart_print_dec(sum_us / ok / 100 % 10); uart_print(" мс"); }
    uart_print("\n");
}

void netcmd_dns(const char* a) {
    while (a && *a == ' ') a++;
    if (!a || !*a) { uart_print("dns имя, например: dns example.com\n"); return; }
    uint32_t ip;
    int r = net_dns(a, &ip);
    uart_print(a);
    if (r == 0) { uart_print(" -> "); print_ip(ip); uart_print("\n"); }
    else uart_print(r == -1 ? ": сети нет (адрес не получен)\n" : r == -4 ? ": такого имени нет\n" : r == -6 ? ": сервер DNS не ответил\n" : ": ответа с адресом нет\n");
}

/* http адрес[:порт] [путь] - запрос GET по TCP; на экран - начало ответа (заголовки и тело), в конце - итог. */
void netcmd_http(const char* a) {
    while (a && *a == ' ') a++;
    if (!a || !*a) { uart_print("http адрес[:порт] [путь], например: http example.com /\n"); return; }
    char host[128]; unsigned n = 0; uint16_t port = 80;
    while (*a && *a != ' ' && *a != ':' && *a != '/' && n + 1 < sizeof host) host[n++] = *a++;
    host[n] = 0;
    if (*a == ':') { a++; uint32_t p = 0; while (*a >= '0' && *a <= '9') p = p * 10 + (uint32_t)(*a++ - '0'); if (p && p < 65536) port = (uint16_t)p; }
    while (*a == ' ') a++;
    const char* path = *a ? a : "/";
    uint32_t ip;
    if (net_dns(host, &ip) != 0) { uart_print("не найден адрес "); uart_print(host); uart_print("\n"); return; }
    uint64_t t0 = clock_ns();
    int h = tcp_connect(ip, port);
    if (h < 0) { uart_print("соединение с "); print_ip(ip); uart_print(" не установлено\n"); return; }
    char req[320]; unsigned k = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nUser-Agent: KiraOS\r\nConnection: close\r\n\r\n" };
    for (unsigned i = 0; i < 5; i++) for (const char* c = parts[i]; *c && k + 1 < sizeof req; c++) req[k++] = *c;
    if (tcp_send(h, req, k) != (int)k) { uart_print("запрос не отправлен\n"); tcp_close(h); return; }
    static uint8_t buf[1024];
    uint64_t total = 0; unsigned shown = 0; int r;
    while ((r = tcp_recv(h, buf, sizeof buf, 10 * TIMER_HZ)) > 0) {
        for (int i = 0; i < r && shown < 1200; i++, shown++) {
            char c = (char)buf[i];
            if (c == '\r') continue;
            if (c != '\n' && c != '\t' && (unsigned char)c < 32) c = '.';
            char s2[2] = { c, 0 }; uart_print(s2);
        }
        total += (uint64_t)r;
    }
    tcp_close(h);
    uint64_t ms = (clock_ns() - t0) / 1000000;
    uart_print(shown >= 1200 ? "\n...\n" : "\n");
    uart_print("получено "); uart_print_dec(total); uart_print(" байт от "); print_ip(ip); uart_print(":"); uart_print_dec(port);
    uart_print(" за "); uart_print_dec(ms); uart_print(" мс");
    uart_print(r == 0 ? " (сервер закрыл соединение)\n" : r == -2 ? " (соединение сброшено)\n" : " (ответ оборвался по времени)\n");
}
#endif
