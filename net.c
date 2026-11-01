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
void netcmd_dns(const char* a) { (void)a; }
void netcmd_http(const char* a) { (void)a; }
void net_proc_gone(uint32_t pid) { (void)pid; }
#else
#include "sched.h"
#include "timer.h"
#include "clock.h"
#include "uart.h"
#include "spinlock.h"
#include "msi.h"

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

/* ---------- TCP (клиент) ----------
   Соединения ядра: установка (SYN, SYN+ACK, ACK), приём по порядку в кольцо с окном по свободному месту, передача
   по одному сегменту с ожиданием подтверждения и повтором (1, 2, 4... с, 6 попыток), закрытие с обеих сторон.
   Вне порядка пришедшее не хранится: повторное подтверждение - отправитель пошлёт заново. */
#define TCP_MAX 4
#define TCP_RX  16384
enum { T_FREE, T_SYN_SENT, T_EST, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_LAST_ACK, T_CLOSED };
enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };
typedef struct {
    int state, reset, fin_rcvd;
    uint32_t rip; uint16_t rport, lport; uint8_t mac[6];
    uint32_t snd_una, snd_nxt, rcv_nxt; uint16_t mss;
    uint8_t tx[1460]; uint32_t tx_len, tx_seq; uint8_t tx_flags; int tx_busy; uint64_t tx_time; unsigned tx_tries;
    uint8_t rx[TCP_RX]; uint32_t rx_head, rx_tail;              /* занято: tail - head */
    uint64_t rx_bytes;
} tcb_t;
static tcb_t tcbs[TCP_MAX];
static spinlock_t tlk = SPINLOCK_INIT;
static uint16_t next_lport = 49152;

static uint16_t tcp_win(const tcb_t* t) { uint32_t f = TCP_RX - (t->rx_tail - t->rx_head); return (uint16_t)(f > 65535 ? 65535 : f); }

static void tcp_out(tcb_t* t, uint8_t flags, uint32_t seq, const uint8_t* data, uint32_t len) {
    uint8_t s[12 + 24 + 1460];
    uint8_t* h = s + 12;                                            /* перед заголовком - псевдозаголовок для суммы */
    uint32_t hl = (flags & F_SYN) ? 24 : 20;
    put32(s, my_ip); put32(s + 4, t->rip); s[8] = 0; s[9] = 6; put16(s + 10, (uint16_t)(hl + len));
    put16(h, t->lport); put16(h + 2, t->rport); put32(h + 4, seq); put32(h + 8, (flags & F_ACK) ? t->rcv_nxt : 0);
    h[12] = (uint8_t)((hl / 4) << 4); h[13] = flags; put16(h + 14, tcp_win(t)); put16(h + 16, 0); put16(h + 18, 0);
    if (flags & F_SYN) { h[20] = 2; h[21] = 4; put16(h + 22, 1460); }   /* MSS */
    cpy(h + hl, data, len);
    put16(h + 16, net_csum(s, 12 + hl + len));
    ip_send(t->mac, my_ip, t->rip, 6, h, hl + len);
}

/* Сегмент, ждущий подтверждения: запомнить и отправить (под tlk). */
static void tcp_queue(tcb_t* t, uint8_t flags, const uint8_t* data, uint32_t len) {
    t->tx_flags = flags; t->tx_len = len; t->tx_seq = t->snd_nxt;
    cpy(t->tx, data, len);
    t->snd_nxt += len + ((flags & (F_SYN | F_FIN)) ? 1 : 0);
    t->tx_busy = 1; t->tx_tries = 0; t->tx_time = timer_get_ticks();
    tcp_out(t, flags, t->tx_seq, t->tx, len);
}

static void tcp_input(uint32_t src, const uint8_t* g, uint32_t len) {
    if (len < 20) return;
    uint32_t hl = (uint32_t)(g[12] >> 4) * 4;
    if (hl < 20 || hl > len) return;
    {   /* сумма с псевдозаголовком */
        static uint8_t c[12 + 1500];
        if (len > 1500) return;
        put32(c, src); put32(c + 4, my_ip); c[8] = 0; c[9] = 6; put16(c + 10, (uint16_t)len);
        cpy(c + 12, g, len);
        if (net_csum(c, 12 + len) != 0) return;
    }
    uint16_t sport = get16(g), dport = get16(g + 2);
    uint32_t seq = get32(g + 4), ack = get32(g + 8);
    uint8_t fl = g[13];
    const uint8_t* data = g + hl; uint32_t dl = len - hl;
    uint64_t f = spin_lock_irqsave(&tlk);
    tcb_t* t = 0;
    for (int i = 0; i < TCP_MAX; i++)
        if (tcbs[i].state != T_FREE && tcbs[i].rip == src && tcbs[i].rport == sport && tcbs[i].lport == dport) t = &tcbs[i];
    if (!t || t->state == T_CLOSED) { spin_unlock_irqrestore(&tlk, f); return; }
    if (fl & F_RST) { t->reset = 1; t->state = T_CLOSED; t->tx_busy = 0; spin_unlock_irqrestore(&tlk, f); return; }
    if (t->state == T_SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1; t->snd_una = ack; t->tx_busy = 0;
            t->mss = 536;
            for (uint32_t i = 20; i + 1 < hl; ) {                   /* MSS собеседника */
                uint8_t k = g[i];
                if (k == 0) break;
                if (k == 1) { i++; continue; }
                if (g[i + 1] < 2) break;
                if (k == 2 && g[i + 1] == 4 && i + 4 <= hl) t->mss = get16(g + i + 2);
                i += g[i + 1];
            }
            if (t->mss > 1460) t->mss = 1460;
            t->state = T_EST;
            tcp_out(t, F_ACK, t->snd_nxt, 0, 0);
        }
        spin_unlock_irqrestore(&tlk, f);
        return;
    }
    if ((fl & F_ACK) && (int32_t)(ack - t->snd_una) > 0 && (int32_t)(ack - t->snd_nxt) <= 0) {
        t->snd_una = ack;
        if (ack == t->snd_nxt) {
            t->tx_busy = 0;
            if (t->state == T_FIN_WAIT1) t->state = T_FIN_WAIT2;
            else if (t->state == T_LAST_ACK) t->state = T_CLOSED;
        }
    }
    int need_ack = 0;
    if (dl) {
        if (seq == t->rcv_nxt) {
            uint32_t room = TCP_RX - (t->rx_tail - t->rx_head), take = dl < room ? dl : room;
            for (uint32_t i = 0; i < take; i++) t->rx[(t->rx_tail + i) % TCP_RX] = data[i];
            t->rx_tail += take; t->rcv_nxt += take; t->rx_bytes += take;
        }
        need_ack = 1;                                               /* и на чужой порядок - повтор подтверждения */
    }
    if ((fl & F_FIN) && seq + dl == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++; t->fin_rcvd = 1; need_ack = 1;
        if (t->state == T_EST) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1 || t->state == T_FIN_WAIT2) t->state = T_CLOSED;   /* TIME_WAIT не держим */
    }
    if (need_ack) tcp_out(t, F_ACK, t->snd_nxt, 0, 0);
    spin_unlock_irqrestore(&tlk, f);
}

/* Повтор неподтверждённого сегмента (поток «net», каждый тик). */
static void tcp_tick(uint64_t now) {
    uint64_t f = spin_lock_irqsave(&tlk);
    for (int i = 0; i < TCP_MAX; i++) {
        tcb_t* t = &tcbs[i];
        if (!t->tx_busy || t->state == T_CLOSED || t->state == T_FREE) continue;
        if (now - t->tx_time < ((uint64_t)TIMER_HZ << t->tx_tries)) continue;
        if (++t->tx_tries > 6) { t->state = T_CLOSED; t->reset = 1; t->tx_busy = 0; continue; }
        t->tx_time = now;
        tcp_out(t, t->tx_flags, t->tx_seq, t->tx, t->tx_len);
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
static int c_sent(const tcb_t* t) { return !t->tx_busy || t->state == T_CLOSED; }
static int c_data(const tcb_t* t) { return t->rx_tail != t->rx_head || t->fin_rcvd || t->state == T_CLOSED; }
static int c_closed(const tcb_t* t) { return t->state == T_CLOSED; }

int tcp_connect(uint32_t ip, uint16_t port) {
    if (!dev || dstate != D_BOUND) return -1;
    uint8_t mac[6];
    if (resolve(ip, mac) != 0) return -2;
    uint64_t f = spin_lock_irqsave(&tlk);
    int h = -1;
    for (int i = 0; i < TCP_MAX && h < 0; i++) if (tcbs[i].state == T_FREE) h = i;
    if (h < 0) { spin_unlock_irqrestore(&tlk, f); return -3; }
    tcb_t* t = &tcbs[h];
    t->reset = t->fin_rcvd = 0; t->rip = ip; t->rport = port; t->lport = next_lport++;
    if (next_lport < 49152) next_lport = 49152;
    cpy(t->mac, mac, 6);
    t->snd_una = t->snd_nxt = (uint32_t)(clock_ns() >> 6); t->rcv_nxt = 0; t->mss = 536;
    t->rx_head = t->rx_tail = 0; t->rx_bytes = 0;
    t->state = T_SYN_SENT;
    tcp_queue(t, F_SYN, 0, 0);
    spin_unlock_irqrestore(&tlk, f);
    if (tcp_wait(t, c_open, 10 * TIMER_HZ) != 0 || t->state != T_EST) { t->state = T_FREE; return -4; }
    return h;
}

int tcp_send(int h, const void* data, uint32_t len) {
    if (h < 0 || h >= TCP_MAX) return -1;
    tcb_t* t = &tcbs[h];
    const uint8_t* p = (const uint8_t*)data;
    uint32_t done = 0;
    while (done < len) {
        if (t->state != T_EST && t->state != T_CLOSE_WAIT) return done ? (int)done : -1;
        uint32_t n = len - done < t->mss ? len - done : t->mss;
        uint64_t f = spin_lock_irqsave(&tlk);
        tcp_queue(t, F_ACK | F_PSH, p + done, n);
        spin_unlock_irqrestore(&tlk, f);
        if (tcp_wait(t, c_sent, 70 * TIMER_HZ) != 0 || t->reset) return -2;
        done += n;
    }
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
    uint64_t f = spin_lock_irqsave(&tlk);
    if (t->state == T_EST) { t->state = T_FIN_WAIT1; tcp_queue(t, F_FIN | F_ACK, 0, 0); }
    else if (t->state == T_CLOSE_WAIT) { t->state = T_LAST_ACK; tcp_queue(t, F_FIN | F_ACK, 0, 0); }
    spin_unlock_irqrestore(&tlk, f);
    tcp_wait(t, c_closed, 3 * TIMER_HZ);
    f = spin_lock_irqsave(&tlk);
    t->state = T_FREE; t->tx_busy = 0;
    spin_unlock_irqrestore(&tlk, f);
}

/* ---------- сеть для программ (системные вызовы) ----------
   Соединение принадлежит процессу, который его открыл: чужой номер - отказ; при завершении процесса его соединения
   закрываются (net_proc_gone). Данные копируются через буфер ядра проверенными копиями (uaccess). */
#include "uaccess.h"
#include "proc.h"
static uint32_t tcp_owner[TCP_MAX];
static int own(int64_t h) { return h >= 0 && h < TCP_MAX && tcp_owner[h] && tcp_owner[h] == proc_current_pid(); }

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
    if (op == NET_SYS_CLOSE) { tcp_owner[h] = 0; tcp_close(h); return 0; }
    return -10;
}

void net_proc_gone(uint32_t pid) {
    for (int i = 0; i < TCP_MAX; i++) if (tcp_owner[i] == pid) { tcp_owner[i] = 0; tcp_close(i); }
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
    else if (p[9] == 17 && n >= 8 && dns_port && get16(d + 2) == dns_port && get16(d) == 53 && src == my_dns) dns_input(d + 8, n - 8);
    else if (p[9] == 6) tcp_input(src, d, n);
}

static void net_main(void* arg) {
    (void)arg;
    static uint8_t f[2048];
    int had_link = -1;
    uint32_t mark = dev->iw ? irqwait_mark(dev->iw) : 0;
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
        tcp_tick(timer_get_ticks());
        if (dev->iw) { irqwait_wait(dev->iw, mark, 1); mark = irqwait_mark(dev->iw); }   /* до прерывания (не дольше тика) */
        else sched_sleep_ticks(1);
    }
}

void net_register(netdev_t* d) { if (!dev) dev = d; }

int net_start(void) {
    if (!dev) { uart_print("[KIRA NET] No supported network card: networking is off.\n"); return 0; }
    if (!sched_create("net", net_main, 0, PRIO_NORMAL)) { uart_print("[KIRA NET] cannot create network thread.\n"); return 0; }
    uart_print("[KIRA NET] Network stack online: Ethernet, ARP, IPv4, ICMP, UDP, DHCP client, DNS, TCP (commands: net, ping, dns, http).\n");
    return 1;
}

/* ---------- команды ---------- */
void netcmd_net(const char* a) {
    (void)a;
    if (!dev) { uart_print("сетевой карты нет (поддерживаются Intel e1000 и virtio-net: в Limbo - Network: User, NIC: e1000 или virtio)\n"); return; }
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
