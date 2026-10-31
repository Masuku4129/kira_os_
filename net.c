/* Сетевой стек ядра: Ethernet, ARP, IPv4, ICMP (эхо: ответ на ping и свой ping), UDP, клиент DHCP.
   Входящие кадры и DHCP обрабатывает поток «net» (опрос карты каждый тик); консоль отправляет ping и ждёт ответа,
   который отмечает поток. Фрагментация IP и TCP - следующий шаг. */
#include "net.h"

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
#else
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "uart.h"
#include "spinlock.h"

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
static int ip_send(const uint8_t* mac, uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* data, uint32_t len) {
    uint8_t p[1500];
    if (len > 1480) return -1;
    p[0] = 0x45; p[1] = 0; put16(p + 2, (uint16_t)(20 + len)); put16(p + 4, ip_ident++); put16(p + 6, 0x4000);  /* DF */
    p[8] = 64; p[9] = proto; put16(p + 10, 0); put32(p + 12, src); put32(p + 16, dst);
    put16(p + 10, net_csum(p, 20));
    cpy(p + 20, data, len);
    return eth_send(mac, 0x0800, p, 20 + len);
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

/* ---------- приём ---------- */
static void icmp_input(const uint8_t* eth_src, uint32_t src, const uint8_t* m, uint32_t len) {
    if (len < 8 || net_csum(m, len) != 0) return;
    if (m[0] == 8 && my_ip) {                                       /* эхо-запрос: ответить тем же */
        uint8_t r[1480];
        if (len > sizeof r) return;
        cpy(r, m, len); r[0] = 0; put16(r + 2, 0); put16(r + 2, net_csum(r, len));
        ip_send(eth_src, my_ip, src, 1, r, len);
    } else if (m[0] == 0 && get16(m + 4) == 0x4B49 && get16(m + 6) == ping_seq && src == ping_ip) {
        ping_rx_ns = clock_ns();
        __atomic_store_n(&ping_got, 1, __ATOMIC_RELEASE);
    }
}

static void ip_input(const uint8_t* eth_src, const uint8_t* p, uint32_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    uint32_t ihl = (uint32_t)(p[0] & 15) * 4, tot = get16(p + 2);
    if (ihl < 20 || tot < ihl || tot > len || net_csum(p, ihl) != 0) return;
    if (get16(p + 6) & 0x3FFF) return;                              /* фрагменты пока не собираются */
    uint32_t src = get32(p + 12), dst = get32(p + 16);
    int to_us = (my_ip && dst == my_ip) || dst == 0xFFFFFFFF || (my_mask && dst == (my_ip | ~my_mask)) || (!my_ip && p[9] == 17);
    if (!to_us) return;
    const uint8_t* d = p + ihl; uint32_t n = tot - ihl;
    if (src && (!my_mask || (src & my_mask) == (my_ip & my_mask))) arp_learn(src, eth_src);
    if (p[9] == 1) icmp_input(eth_src, src, d, n);
    else if (p[9] == 17 && n >= 8 && get16(d + 2) == 68 && get16(d) == 67) dhcp_input(d + 8, n - 8);
}

static void net_main(void* arg) {
    (void)arg;
    static uint8_t f[2048];
    int had_link = -1;
    for (;;) {
        int n;
        while ((n = dev->recv(dev, f, sizeof f)) > 0) {
            rx_frames++;
            if (n < 14) { rx_drop++; continue; }
            uint16_t type = get16(f + 12);
            if (type == 0x0806) arp_input(f + 14, (uint32_t)n - 14);
            else if (type == 0x0800) ip_input(f + 6, f + 14, (uint32_t)n - 14);
        }
        int l = dev->link(dev);
        if (l != had_link) {
            if (had_link >= 0) uart_print(l ? "[KIRA NET] link up\n" : "[KIRA NET] link down\n");
            had_link = l;
            if (!l) { my_ip = 0; dstate = D_INIT; }
        }
        dhcp_tick(timer_get_ticks());
        sched_sleep_ticks(1);
    }
}

void net_register(netdev_t* d) { if (!dev) dev = d; }

int net_start(void) {
    if (!dev) { uart_print("[KIRA NET] No supported network card: networking is off.\n"); return 0; }
    if (!sched_create("net", net_main, 0, PRIO_NORMAL)) { uart_print("[KIRA NET] cannot create network thread.\n"); return 0; }
    uart_print("[KIRA NET] Network stack online: Ethernet, ARP, IPv4, ICMP, UDP, DHCP client (commands: net, ping).\n");
    return 1;
}

/* ---------- команды ---------- */
void netcmd_net(const char* a) {
    (void)a;
    if (!dev) { uart_print("сетевой карты нет (поддерживается Intel e1000: в Limbo - Network: User, NIC: e1000)\n"); return; }
    uart_print("карта "); uart_print(dev->name); uart_print(", канал "); uart_print(dev->link(dev) ? "есть" : "нет");
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
#endif
