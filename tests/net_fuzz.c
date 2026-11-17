/* Фаззинг приёма сетевых кадров ядра под ASan/UBSan (пункт А6 плана): настоящий стек net.c, ip6.c, ipfrag.c на хосте.
   net.c включён в этот файл целиком (KIRA_NET_HOST): так тест видит состояние стека и сбрасывает его между сценариями,
   а ядерные вызовы (планировщик, таймер, куча, копирование из программ, UART) подменены ниже. Кадры подаёт net_rx_frame -
   та же функция, что вызывает поток «net».

   Что подаётся: образцы настоящих кадров, собранные здесь же (ARP, ICMP эхо, UDP, DHCP, DNS с указателями сжатия, TCP всех
   флагов, IPv6 ND/RA/ICMPv6/расширения/фрагменты, фрагменты IPv4), их искажения (байт, бит, обрезка, граничные значения
   длин и смещений, неверные суммы, чинимые суммы - чтобы дойти вглубь разбора), чистая случайность; разговоры по TCP и
   DNS с «собеседником», который отвечает на то, что стек отправил; сборка фрагментов в случайном порядке с проверкой, что
   датаграмма дошла целой.
   Инварианты на каждом кадре, который стек отправил карте: длина не больше MTU, версия/длины/флаги заголовков верны,
   контрольные суммы IPv4, ICMP, ICMPv6, UDP, TCP верны (фрагменты собираются и проверяются целиком), адрес отправителя
   свой, типы сообщений известные; плюс внутренние границы (кольца, окна, сборка фрагментов), отсутствие утечек кучи ядра,
   время одного входа ограничено (сторожевой таймер).
   Использование: net_fuzz СЦЕНАРИЕВ ЗЁРНО [С_НОМЕРА]   (сценарий N воспроизводится по зерну и номеру: net_fuzz 1 ЗЁРНО N) */
#define KIRA_NET_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>
#include "../net.c"
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#else
#define ASAN_POISON_MEMORY_REGION(a, n) ((void)(a), (void)(n))
#define ASAN_UNPOISON_MEMORY_REGION(a, n) ((void)(a), (void)(n))
#endif

/* ---------- подмены ядра ---------- */
static uint64_t g_ticks, g_ns;
static void (*g_hook)(void);                                  /* зовётся при каждом сне: «собеседник» отвечает, пока стек ждёт */
static long g_live;                                           /* живых блоков кучи ядра (утечки) */
uint64_t timer_get_ticks(void) { return g_ticks; }
uint64_t clock_ns(void) { g_ns += 7919; return g_ns; }
void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
thread_t* sched_create(const char* n, void (*e)(void*), void* a, thread_prio_t p) { (void)n; (void)e; (void)a; (void)p; return 0; }
void sched_sleep_ticks(uint32_t t) { g_ticks += t; if (g_hook) g_hook(); }
void irqwait_wait(irqwait_t* w, uint32_t m, uint64_t t) { (void)w; (void)m; (void)t; }
void* kmalloc(size_t n) { void* p = malloc(n ? n : 1); if (p) g_live++; return p; }
void kfree(void* p) { if (p) { g_live--; free(p); } }
int copy_from_user(void* d, uint64_t s, uint64_t n) { memcpy(d, (const void*)(uintptr_t)s, n); return 0; }
int copy_to_user(uint64_t d, const void* s, uint64_t n) { memcpy((void*)(uintptr_t)d, s, n); return 0; }
int user_range(uint64_t va, uint64_t len, int w) { (void)w; return va != 0 || len == 0; }
uint32_t proc_current_pid(void) { return 1; }

/* ---------- случайность, отчёт ---------- */
static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t rr(uint32_t n) { return n ? rnd() % n : 0; }
static int chance(uint32_t pct) { return rr(100) < pct; }
static uint64_t g_seed, g_scen, g_inputs, g_sent, g_hist_n;
static int fails;
#define HIST 24
static uint8_t g_hist[HIST][2048]; static uint32_t g_histn[HIST];     /* последние поданные кадры (для отчёта о сбое) */
static void dump_hist(void) {
    fprintf(stderr, "сценарий %llu, зерно (параметр) %llu; повтор: net_fuzz 1 ЗЁРНО НОМЕР; последние поданные кадры (первые 96 байт):\n", (unsigned long long)g_scen, (unsigned long long)(g_seed ^ 0x2545f4914f6cdd1dull));
    for (uint64_t i = g_hist_n > 6 ? g_hist_n - 6 : 0; i < g_hist_n; i++) {
        fprintf(stderr, "  [%u] ", g_histn[i % HIST]);
        for (uint32_t k = 0; k < g_histn[i % HIST] && k < 96; k++) fprintf(stderr, "%02x", g_hist[i % HIST][k]);
        fprintf(stderr, "\n");
    }
}
#define BAD(...) do { fprintf(stderr, "FAIL (%s:%d): ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); if (!fails) dump_hist(); fails++; } while (0)
static void on_fatal(int sig) { fprintf(stderr, "\nСИГНАЛ %d (падение или зависание)\n", sig); dump_hist(); _exit(sig == SIGALRM ? 3 : 4); }

/* ---------- карта-подмена и проверка отправленного ---------- */
static const uint8_t g_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t PEER_MAC[6] = { 0x02, 0, 0, 0, 0, 0x02 };
#define MY_IP   0x0A00020Fu
#define PEER_IP 0x0A000202u
#define DNS_IP  0x0A000203u
#define FAR_IP  0x5DB8D822u
static uint8_t PEER6[16] = { 0xFE, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
static uint8_t FAR6[16] = { 0x20, 0x01, 0x0D, 0xB8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x99 };
static uint8_t PFX6[16] = { 0x20, 0x01, 0x0D, 0xB8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static uint8_t g_ll[16], g_gl[16];                           /* наши адреса IPv6: канала и SLAAC */
static int g_bound4, g_v6ready;

#define OUT_N 64
static uint8_t g_out[OUT_N][1600]; static uint32_t g_outn[OUT_N]; static uint64_t g_nout;
static uint8_t g_dg[65536]; static uint32_t g_dgn; static int g_dgproto, g_dgcount;   /* последняя собранная из фрагментов отправленная датаграмма */
static struct { int used; uint16_t id; uint8_t proto; uint32_t got, total; uint8_t buf[65536]; } g_ra;

static void check_l4(int fam, const uint8_t* src, const uint8_t* dst, uint8_t proto, const uint8_t* d, uint32_t n, int ihl_unfrag) {
    (void)ihl_unfrag;
    if (proto == 1) {
        if (n < 8 || net_csum(d, n) != 0) BAD("ICMP: сумма или длина (%u)", n);
        if (d[0] != 0 && d[0] != 8) BAD("ICMP: неожиданный тип %u", d[0]);
    } else if (proto == 17) {
        if (n < 8 || get16(d + 4) != n) { BAD("UDP: поле длины %u при %u", n >= 8 ? get16(d + 4) : 0, n); return; }
        if (get16(d + 6) || fam == 6) {
            uint16_t c = fam == 4 ? net_csum4(get32(src), get32(dst), 17, d, n) : net_csum6(src, dst, 17, d, n);
            if (c != 0) BAD("UDP: неверная сумма");
        }
        if (fam == 6 && !get16(d + 6)) BAD("UDP по IPv6 без суммы");
    } else if (proto == 6) {
        if (n < 20) { BAD("TCP: короче заголовка"); return; }
        uint32_t hl = (uint32_t)(d[12] >> 4) * 4;
        if (hl < 20 || hl > n) BAD("TCP: смещение данных %u при %u", hl, n);
        uint16_t c = fam == 4 ? net_csum4(get32(src), get32(dst), 6, d, n) : net_csum6(src, dst, 6, d, n);
        if (c != 0) BAD("TCP: неверная сумма");
        uint8_t fl = d[13];
        if ((fl & F_SYN) && (fl & (F_FIN | F_RST))) BAD("TCP: SYN вместе с FIN/RST");
    } else if (proto == 58) {
        if (n < 8 || net_csum6(src, dst, 58, d, n) != 0) BAD("ICMPv6: сумма или длина");
        uint8_t t = d[0];
        if (t != 128 && t != 129 && t != 133 && t != 135 && t != 136) BAD("ICMPv6: неожиданный тип %u", t);
        if (t >= 133 && t <= 136 && n < (t == 133 ? 8u : 24u)) BAD("ND: короткое сообщение");
    } else BAD("отправлен неожиданный протокол %u", proto);
}

static void check_sent(const uint8_t* f, uint32_t n) {
    if (n < 14 || n > 1514) { BAD("отправлен кадр длиной %u (MTU 1500)", n); return; }
    if (memcmp(f + 6, g_mac, 6) != 0) BAD("MAC отправителя не наш");
    uint16_t type = get16(f + 12);
    const uint8_t* p = f + 14; uint32_t len = n - 14;
    if (type == 0x0806) {
        if (len != 28 || get16(p) != 1 || get16(p + 2) != 0x0800 || p[4] != 6 || p[5] != 4) { BAD("ARP: неверный кадр"); return; }
        if (get16(p + 6) != 1 && get16(p + 6) != 2) BAD("ARP: код %u", get16(p + 6));
        if (memcmp(p + 8, g_mac, 6) != 0) BAD("ARP: наш MAC в поле отправителя неверен");
    } else if (type == 0x0800) {
        if (len < 20 || p[0] != 0x45) { BAD("IPv4: версия/длина заголовка %02x, длина %u", len ? p[0] : 0, len); return; }
        uint32_t tot = get16(p + 2), src = get32(p + 12), dst = get32(p + 16);
        if (tot != len) { BAD("IPv4: общая длина %u, кадр несёт %u", tot, len); return; }
        if (net_csum(p, 20) != 0) BAD("IPv4: сумма заголовка");
        if (p[8] == 0) BAD("IPv4: TTL 0");
        if (src != my_ip && !(src == 0 && p[9] == 17)) BAD("IPv4: адрес отправителя %08x не наш (%08x)", src, my_ip);
        if (src == 0xFFFFFFFFu || (src >> 28) == 0xE) BAD("IPv4: отправитель - групповой адрес %08x", src);
        if (dst == 0) BAD("IPv4: получатель 0.0.0.0");
        uint16_t fo = get16(p + 6); uint32_t pn = tot - 20; const uint8_t* d = p + 20;
        uint8_t sa[4], da[4]; put32(sa, src); put32(da, dst);
        if (!(fo & 0x3FFF)) {
            if (fo != 0x4000) BAD("IPv4: целая датаграмма без DF (%04x)", fo);
            check_l4(4, sa, da, p[9], d, pn, 1);
            memcpy(g_dg, d, pn); g_dgn = pn; g_dgproto = p[9]; g_dgcount++;
        } else {
            uint32_t off = (uint32_t)(fo & 0x1FFF) * 8; int mf = (fo & 0x2000) != 0;
            if (fo & 0x4000) BAD("IPv4: фрагмент с DF");
            if (mf && (pn & 7)) BAD("IPv4: не последний фрагмент не кратен 8 (%u)", pn);
            if (pn == 0 || pn > 1480) BAD("IPv4: фрагмент %u байт", pn);
            if (off + pn > 65515) { BAD("IPv4: фрагмент за пределом 65515"); return; }
            if (!g_ra.used || g_ra.id != get16(p + 4) || g_ra.proto != p[9]) { g_ra.used = 1; g_ra.id = get16(p + 4); g_ra.proto = p[9]; g_ra.got = 0; g_ra.total = 0; }
            memcpy(g_ra.buf + off, d, pn); g_ra.got += pn;
            if (!mf) g_ra.total = off + pn;
            if (g_ra.total && g_ra.got > g_ra.total) BAD("IPv4: фрагменты перекрываются");
            if (g_ra.total && g_ra.got == g_ra.total) {
                check_l4(4, sa, da, g_ra.proto, g_ra.buf, g_ra.total, 0);
                memcpy(g_dg, g_ra.buf, g_ra.total); g_dgn = g_ra.total; g_dgproto = g_ra.proto; g_dgcount++;
                g_ra.used = 0;
            }
        }
    } else if (type == 0x86DD) {
        if (len < 40 || p[0] != 0x60) { BAD("IPv6: версия/длина"); return; }
        uint32_t pl = get16(p + 4);
        if (pl + 40 != len) { BAD("IPv6: длина данных %u, кадр несёт %u", pl, len - 40); return; }
        if (len > mtu6) BAD("IPv6: пакет %u больше MTU %u", len, mtu6);
        if (ip6_is_mcast(p + 8)) BAD("IPv6: групповой отправитель");
        if (ip6_is_unspec(p + 24)) BAD("IPv6: получатель ::");
        uint8_t nh = p[6];
        if (nh != 58 && nh != 6 && nh != 17) { BAD("IPv6: следующий заголовок %u", nh); return; }
        if (nh == 58 && p[40] >= 133 && p[40] <= 136 && p[7] != 255) BAD("ND с лимитом хопов %u (нужно 255)", p[7]);
        if (p[7] == 0) BAD("IPv6: лимит хопов 0");
        check_l4(6, p + 8, p + 24, nh, p + 40, pl, 1);
    } else BAD("отправлен кадр неизвестного типа %04x", type);
}

static int mock_send(netdev_t* d, const void* fr, uint32_t len) {
    (void)d;
    check_sent((const uint8_t*)fr, len);
    uint32_t k = (uint32_t)(g_nout % OUT_N);
    g_outn[k] = len > 1600 ? 1600 : len; memcpy(g_out[k], fr, g_outn[k]);
    g_nout++; g_sent++;
    return 0;
}
static int mock_recv(netdev_t* d, void* b, uint32_t m) { (void)d; (void)b; (void)m; return 0; }
static int mock_link(netdev_t* d) { (void)d; return 1; }
static int mock_mcast(netdev_t* d, const uint8_t m[6]) { (void)d; (void)m; return 0; }
static netdev_t g_dev = { "fuzz0", { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }, mock_send, mock_recv, mock_link, 0, 0, mock_mcast };

/* ---------- сброс стека ---------- */
static void reset_all(void) {
    dev = &g_dev;
    my_ip = my_mask = my_gw = my_dns = lease_s = 0;
    rx_frames = tx_frames = rx_drop = 0;
    memset(&lk, 0, sizeof lk); memset(&lk6, 0, sizeof lk6); memset(&tlk, 0, sizeof tlk); memset(&ulk, 0, sizeof ulk);
    memset(arp, 0, sizeof arp);
    ping_seq = 0; ping_ip = 0; ping_got = 0; ping_rx_ns = 0; ip_ident = 0;
    memset(a6, 0, sizeof a6); memset(pfx6, 0, sizeof pfx6); memset(gw6, 0, sizeof gw6); memset(dns6, 0, sizeof dns6);
    gw6_set = dns6_set = 0; gw6_until = 0; mtu6 = 1500; v6_on = v6_dup = ra_seen = 0; rs_at = rs_end = 0; rs_left = 0;
    memset(nd6, 0, sizeof nd6); v6_drop = 0;
    ping6_seq = 0; memset(ping6_ip, 0, sizeof ping6_ip); ping6_got = 0; ping6_rx_ns = 0;
    dstate = D_INIT; xid = offer_ip = offer_srv = 0; dnext = dbound_at = 0; dtries = 0;
    dns_port = dns_id = 0; ASAN_UNPOISON_MEMORY_REGION(dns_buf, sizeof dns_buf); memset(dns_buf, 0, sizeof dns_buf); dns_len = 0; dns_got = 0;
    for (int i = 0; i < TCP_MAX; i++) memset(&tcbs[i], 0, offsetof(tcb_t, tx));        /* кольца данных не обнуляем: ими владеют tx_len/rx_head/rx_tail, их tcb_new сбрасывает */
    memset(listens, 0, sizeof listens); next_lport = 49152;
    memset(tw, 0, sizeof tw); tw_total = 0;
    memset(tcp_owner, 0, sizeof tcp_owner); memset(listen_owner, 0, sizeof listen_owner);
    for (int i = 0; i < UDP_MAX; i++) memset(&us[i], 0, offsetof(__typeof__(us[0]), ring));
    next_uport = 50000;
    ipfrag_init(&frag, 30 * TIMER_HZ, 2 * TIMER_HZ);
    g_nout = 0; g_ra.used = 0; g_dgn = 0; g_dgcount = 0; g_hook = 0;
    g_bound4 = g_v6ready = 0;
}

static void host_tick(void) {                                  /* то же, что делает net_main после разбора кадров */
    dhcp_tick(g_ticks); tcp_tick(g_ticks); v6_tick(g_ticks); ipfrag_expire(&frag, g_ticks);
}

/* ---------- сборщики кадров ---------- */
static uint32_t eth_wrap(uint8_t* f, uint16_t type, const uint8_t* pl, uint32_t n) {
    if (n > 2034) n = 2034;
    cpy(f, g_mac, 6); cpy(f + 6, PEER_MAC, 6); put16(f + 12, type); cpy(f + 14, pl, n);
    return 14 + n;
}
static uint32_t ip4_build(uint8_t* o, uint32_t src, uint32_t dst, uint8_t proto, uint16_t id, uint16_t fo, const uint8_t* pl, uint32_t n) {
    o[0] = 0x45; o[1] = 0; put16(o + 2, (uint16_t)(20 + n)); put16(o + 4, id); put16(o + 6, fo); o[8] = 64; o[9] = proto;
    put16(o + 10, 0); put32(o + 12, src); put32(o + 16, dst); put16(o + 10, net_csum(o, 20)); cpy(o + 20, pl, n);
    return 20 + n;
}
static uint32_t ip6_build(uint8_t* o, const uint8_t* src, const uint8_t* dst, uint8_t nh, uint8_t hop, const uint8_t* pl, uint32_t n) {
    o[0] = 0x60; o[1] = o[2] = o[3] = 0; put16(o + 4, (uint16_t)n); o[6] = nh; o[7] = hop; cpy(o + 8, src, 16); cpy(o + 24, dst, 16); cpy(o + 40, pl, n);
    return 40 + n;
}
static uint32_t udp_build(uint8_t* o, const ipa_t* s, const ipa_t* d, uint16_t sp, uint16_t dp, const uint8_t* data, uint32_t n, int withsum) {
    put16(o, sp); put16(o + 2, dp); put16(o + 4, (uint16_t)(8 + n)); put16(o + 6, 0); cpy(o + 8, data, n);
    if (withsum || s->fam == 6) {
        uint16_t c = s->fam == 4 ? net_csum4(ipa_to4(s), ipa_to4(d), 17, o, 8 + n) : net_csum6(s->a, d->a, 17, o, 8 + n);
        put16(o + 6, c ? c : 0xFFFF);
    }
    return 8 + n;
}
static uint32_t tcp_build(uint8_t* o, const ipa_t* s, const ipa_t* d, uint16_t sp, uint16_t dp, uint32_t seq, uint32_t ack, uint8_t fl, uint16_t win,
                          const uint8_t* opt, uint32_t optn, const uint8_t* data, uint32_t n) {
    uint32_t hl = 20 + optn;
    put16(o, sp); put16(o + 2, dp); put32(o + 4, seq); put32(o + 8, ack); o[12] = (uint8_t)((hl / 4) << 4); o[13] = fl; put16(o + 14, win);
    put16(o + 16, 0); put16(o + 18, 0); if (optn) cpy(o + 20, opt, optn); cpy(o + hl, data, n);
    put16(o + 16, s->fam == 4 ? net_csum4(ipa_to4(s), ipa_to4(d), 6, o, hl + n) : net_csum6(s->a, d->a, 6, o, hl + n));
    return hl + n;
}
/* пакет любого семейства в кадр: транспорт pl, протокол proto */
static uint32_t l3_frame(uint8_t* f, const ipa_t* s, const ipa_t* d, uint8_t proto, const uint8_t* pl, uint32_t n) {
    uint8_t b[2048];
    static uint16_t id;
    uint32_t m = s->fam == 4 ? ip4_build(b, ipa_to4(s), ipa_to4(d), proto, ++id, 0x4000, pl, n) : ip6_build(b, s->a, d->a, proto, 64, pl, n);
    return eth_wrap(f, s->fam == 4 ? 0x0800 : 0x86DD, b, m);
}

#define NIN 48
static uint8_t g_in[NIN][2048]; static uint32_t g_inlen[NIN]; static unsigned g_nin;
static uint8_t* newf(void) { return g_in[g_nin]; }
static void donef(uint32_t n) { g_inlen[g_nin++] = n; }

static ipa_t a4(uint32_t v) { return ipa_v4(v); }
static ipa_t a6x(const uint8_t* v) { return ipa_v6(v); }
static uint32_t pick4(void) { uint32_t r = rr(12); return r < 7 ? PEER_IP : r < 9 ? FAR_IP : r == 9 ? 0 : r == 10 ? 0xFFFFFFFFu : 0xE0000001u; }
static uint32_t pick4dst(void) {
    uint32_t r = rr(10);
    return r < 6 ? MY_IP : r == 6 ? 0xFFFFFFFFu : r == 7 ? 0x0A0002FFu : r == 8 ? 0 : PEER_IP;
}
static const uint8_t* pick6(void) { uint32_t r = rr(5); return r < 2 ? PEER6 : r < 4 ? FAR6 : ALL_NODES; }
static const uint8_t* pick6dst(void) { uint32_t r = rr(8); return r < 3 ? g_ll : r < 6 ? g_gl : r == 6 ? ALL_NODES : ALL_ROUTERS; }

/* DHCP: пакет BOOTP + опции (иногда с мусором) */
static uint32_t dhcp_pkt(uint8_t* o, uint8_t op, uint32_t xi, uint32_t yi, int mt) {
    memset(o, 0, 600);
    o[0] = op; o[1] = 1; o[2] = 6; put32(o + 4, xi); put32(o + 16, yi); cpy(o + 28, g_mac, 6); put32(o + 236, chance(94) ? 0x63825363u : rnd());
    uint8_t* p = o + 240;
    *p++ = 53; *p++ = 1; *p++ = (uint8_t)mt;
    if (chance(70)) { *p++ = 1; *p++ = (uint8_t)(chance(90) ? 4 : rr(8)); put32(p, chance(80) ? 0xFFFFFF00u : rnd()); p += 4; }
    if (chance(60)) { *p++ = 3; *p++ = (uint8_t)(chance(90) ? 4 : rr(9)); put32(p, PEER_IP); p += 4; }
    if (chance(60)) { *p++ = 6; *p++ = 4; put32(p, DNS_IP); p += 4; }
    if (chance(50)) { *p++ = 54; *p++ = 4; put32(p, PEER_IP); p += 4; }
    if (chance(50)) { *p++ = 51; *p++ = 4; put32(p, chance(70) ? 3600 : rnd()); p += 4; }
    if (chance(25)) { *p++ = (uint8_t)rr(256); uint32_t l = rr(60); *p++ = (uint8_t)l; for (uint32_t i = 0; i < l; i++) *p++ = (uint8_t)rnd(); }
    if (chance(15)) { *p++ = 0; *p++ = 0; }
    if (chance(90)) *p++ = 255;
    return (uint32_t)(p - o) + (chance(30) ? rr(30) : 0);
}

/* DNS: ответ с указателями сжатия (в том числе петлями и указателями за границу) */
static uint32_t dns_pkt(uint8_t* o, uint16_t id, int an_n) {
    uint8_t* p = o;
    put16(p, id); put16(p + 2, chance(90) ? 0x8180 : (uint16_t)rnd()); put16(p + 4, chance(90) ? 1 : (uint16_t)rr(4)); put16(p + 6, (uint16_t)an_n); put16(p + 8, 0); put16(p + 10, 0);
    p += 12;
    static const uint8_t qn[] = { 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0 };
    cpy(p, qn, sizeof qn); p += sizeof qn; put16(p, 1); put16(p + 2, 1); p += 4;
    for (int i = 0; i < an_n && p - o < 400; i++) {
        switch (rr(7)) {
        case 0: case 1: *p++ = 0xC0; *p++ = 0x0C; break;                                   /* указатель на вопрос */
        case 2: { uint8_t me = (uint8_t)(p - o); *p++ = 0xC0; *p++ = me; break; }                          /* на самого себя - петля */
        case 3: *p++ = 0xC0; *p++ = (uint8_t)rnd(); break;                                /* куда угодно */
        case 4: *p++ = 3; *p++ = 'f'; *p++ = 'o'; *p++ = 'o'; *p++ = 0xC0; *p++ = 0x0C; break;   /* метки + указатель */
        case 5: *p++ = (uint8_t)(0x40 | rr(0x40)); break;                                 /* недопустимые биты метки */
        default: *p++ = (uint8_t)rr(70); break;                                           /* метка, возможно длиннее остатка */
        }
        uint32_t t = chance(70) ? 1 : chance(50) ? 5 : rr(40);
        put16(p, (uint16_t)t); put16(p + 2, chance(90) ? 1 : (uint16_t)rr(4)); put32(p + 4, rnd());
        uint32_t rd = chance(75) ? (t == 1 ? 4 : rr(20)) : rr(600);
        put16(p + 8, (uint16_t)rd); p += 10;
        for (uint32_t k = 0; k < rd && k < 24; k++) *p++ = (uint8_t)rnd();
    }
    uint32_t total = (uint32_t)(p - o);
    if (chance(30) && total > 12) total -= 1 + rr(total - 12 < 24 ? total - 12 : 24);   /* ответ оборван в любом месте: имя, указатель, запись */
    return total;
}

/* RA с произвольным набором опций */
static uint32_t ra_pkt(uint8_t* o, const uint8_t* src, const uint8_t* dst) {
    memset(o, 0, 400);
    o[0] = 134; o[4] = (uint8_t)rr(256); o[5] = (uint8_t)rr(256);
    put16(o + 6, chance(75) ? 1800 : chance(50) ? 0 : (uint16_t)rnd()); put32(o + 8, rnd()); put32(o + 12, rnd());
    uint32_t n = 16;
    int k = (int)rr(6);
    while (k-- && n + 40 < 400) {
        switch (rr(5)) {
        case 0: o[n] = 1; o[n + 1] = 1; cpy(o + n + 2, PEER_MAC, 6); n += 8; break;
        case 1: case 2:
            o[n] = 3; o[n + 1] = (uint8_t)(chance(90) ? 4 : rr(6)); o[n + 2] = (uint8_t)(chance(80) ? 64 : rr(256)); o[n + 3] = (uint8_t)(chance(70) ? 0xC0 : rr(256));
            put32(o + n + 4, chance(70) ? 86400 : chance(50) ? 0xFFFFFFFFu : rnd()); put32(o + n + 8, chance(70) ? 14400 : rnd());
            cpy(o + n + 16, chance(80) ? PFX6 : FAR6, 16); n += o[n + 1] ? (uint32_t)o[n + 1] * 8 : 8; break;
        case 3: o[n] = 5; o[n + 1] = 1; put32(o + n + 4, chance(70) ? 1280 + rr(221) : rnd()); n += 8; break;
        default: o[n] = 25; o[n + 1] = (uint8_t)(chance(80) ? 3 : rr(6)); put32(o + n + 4, rnd()); cpy(o + n + 8, FAR6, 16); n += o[n + 1] ? (uint32_t)o[n + 1] * 8 : 8; break;
        }
    }
    if (chance(10)) n += rr(8);
    put16(o + 2, 0); put16(o + 2, net_csum6(src, dst, 58, o, n));
    return n;
}

static uint32_t tcp_opts(uint8_t* o) {                         /* параметры TCP: MSS, мусор, обрывы */
    uint32_t n = 0, k = rr(8);
    switch (k) {
    case 0: break;
    case 1: o[0] = 2; o[1] = 4; put16(o + 2, 1460); n = 4; break;
    case 2: o[0] = 2; o[1] = 4; put16(o + 2, (uint16_t)rr(3) * 0x8000); n = 4; break;       /* MSS 0, 32768, 65535... */
    case 3: o[0] = 1; o[1] = 1; o[2] = 3; o[3] = 3; o[4] = 7; o[5] = 0; o[6] = 0; o[7] = 0; n = 8; break;
    case 4: o[0] = 2; o[1] = 0; o[2] = 0; o[3] = 0; n = 4; break;                             /* длина опции 0 */
    case 6: o[0] = 1; o[1] = 1; o[2] = 2; o[3] = 4; n = 4; break;                             /* MSS обрывается на границе заголовка */
    case 7: o[0] = 1; o[1] = 1; o[2] = 1; o[3] = 2; n = 4; break;                             /* тип без длины в конце */
    default: n = 4 * (1 + rr(10)); for (uint32_t i = 0; i < n; i++) o[i] = (uint8_t)rnd(); break;
    }
    return n;
}

/* Разбить датаграмму d (n байт) на фрагменты IPv4 с общим id; результат - кадры g_in; порядок и повторы - как попросили. */
static void frag4(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* d, uint32_t n, int shuffle, int dup, int mangle) {
    uint16_t id = (uint16_t)rnd(); unsigned first = g_nin;
    for (uint32_t off = 0; off < n && g_nin - first < 36; ) {
        uint32_t c = 8 * (1 + rr(190)); int more = 1;
        if (n - off > c * (36 - (g_nin - first))) c = ((n - off) / (36 - (g_nin - first)) + 8) & ~7u;       /* не больше 36 кадров */
        if (off + c >= n) { c = n - off; more = 0; }
        uint8_t b[1600]; uint16_t fo = (uint16_t)((more ? 0x2000 : 0) | (off / 8));
        uint32_t m = ip4_build(b, src, dst, proto, id, fo, d + off, c);
        donef(eth_wrap(newf(), 0x0800, b, m));
        off += c;
    }
    unsigned cnt = g_nin - first;
    if (shuffle) for (unsigned i = cnt; i > 1; i--) { unsigned j = rr(i), a = first + i - 1, b = first + j; uint8_t t[2048]; memcpy(t, g_in[a], 2048); memcpy(g_in[a], g_in[b], 2048); memcpy(g_in[b], t, 2048); uint32_t l = g_inlen[a]; g_inlen[a] = g_inlen[b]; g_inlen[b] = l; }
    if (dup && cnt > 1 && g_nin < NIN) { unsigned s = first + rr(cnt); memcpy(g_in[g_nin], g_in[s], 2048); g_inlen[g_nin] = g_inlen[s]; g_nin++; }
    if (mangle && cnt) {                                                                  /* сдвиг смещения, чужая длина, перекрытие */
        unsigned s = first + rr(cnt); uint8_t* ip = g_in[s] + 14;
        if (rr(2)) put16(ip + 6, (uint16_t)(get16(ip + 6) + (uint16_t)(rr(5) - 2))); else put16(ip + 2, (uint16_t)(get16(ip + 2) + rr(9) - 4));
        put16(ip + 10, 0); put16(ip + 10, net_csum(ip, 20));
    }
}
/* то же для IPv6: заголовок фрагмента (nh = протокол данных), порции кратны 8 */
static void frag6(const uint8_t* src, const uint8_t* dst, uint8_t proto, const uint8_t* d, uint32_t n, int shuffle, int dup, int mangle) {
    uint32_t id = rnd(); unsigned first = g_nin;
    for (uint32_t off = 0; off < n && g_nin - first < 36; ) {
        uint32_t c = 8 * (1 + rr(180)); int more = 1;
        if (n - off > c * (36 - (g_nin - first))) c = ((n - off) / (36 - (g_nin - first)) + 8) & ~7u;
        if (off + c >= n) { c = n - off; more = 0; }
        uint8_t b[1600], pl[1600];
        pl[0] = proto; pl[1] = 0; put16(pl + 2, (uint16_t)(off | (more ? 1 : 0))); put32(pl + 4, id); cpy(pl + 8, d + off, c);
        uint32_t m = ip6_build(b, src, dst, 44, 64, pl, 8 + c);
        donef(eth_wrap(newf(), 0x86DD, b, m));
        off += c;
    }
    unsigned cnt = g_nin - first;
    if (shuffle) for (unsigned i = cnt; i > 1; i--) { unsigned j = rr(i), a = first + i - 1, b = first + j; uint8_t t[2048]; memcpy(t, g_in[a], 2048); memcpy(g_in[a], g_in[b], 2048); memcpy(g_in[b], t, 2048); uint32_t l = g_inlen[a]; g_inlen[a] = g_inlen[b]; g_inlen[b] = l; }
    if (dup && cnt > 1 && g_nin < NIN) { unsigned s = first + rr(cnt); memcpy(g_in[g_nin], g_in[s], 2048); g_inlen[g_nin] = g_inlen[s]; g_nin++; }
    if (mangle && cnt) { unsigned s = first + rr(cnt); uint8_t* fh = g_in[s] + 14 + 40; put16(fh + 2, (uint16_t)(get16(fh + 2) + (uint16_t)(rr(3) * 8) - 8 + (rr(2) ? 1 : 0))); }
}

/* ---------- искажения ---------- */
static const uint32_t EDGE[] = { 0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 19, 20, 21, 23, 24, 27, 28, 39, 40, 41, 0x7F, 0x80, 0xFF, 0x100, 0x1FFF, 0x2000, 0x3FFF,
                                 0x4000, 0x5DC, 0x5DD, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu };
static uint32_t pick_off(uint32_t n) {                         /* чаще - в заголовки */
    static const uint16_t hot[] = { 282, 283, 62, 70, 82, 86, 102, 14, 16, 17, 18, 19, 20, 21, 22, 23, 26, 30, 34, 36, 38, 40, 42, 44, 46, 47, 48, 50, 54, 56, 58, 62, 66, 74, 78 };
    if (n && chance(70)) { uint32_t o = hot[rr(sizeof hot / sizeof hot[0])]; if (o < n) return o; }
    return n ? rr(n) : 0;
}
/* чинит суммы заголовков и транспорта, где можно разобрать: так искажённое проходит проверку суммы и идёт глубже */
static void fix_sums(uint8_t* f, uint32_t n) {
    if (n < 34) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0800 && (f[14] >> 4) == 4) {
        uint32_t ihl = (uint32_t)(f[14] & 15) * 4, tot = get16(f + 16);
        if (ihl < 20 || 14 + ihl > n) return;
        put16(f + 24, 0); put16(f + 24, net_csum(f + 14, ihl));
        if ((get16(f + 20) & 0x3FFF) || tot < ihl || 14 + tot > n) return;
        uint8_t* d = f + 14 + ihl; uint32_t dn = tot - ihl, proto = f[23];
        if (proto == 1 && dn >= 4) { put16(d + 2, 0); put16(d + 2, net_csum(d, dn)); }
        else if (proto == 17 && dn >= 8) { put16(d + 6, 0); uint16_t c = net_csum4(get32(f + 26), get32(f + 30), 17, d, dn); put16(d + 6, c ? c : 0xFFFF); }
        else if (proto == 6 && dn >= 20) { put16(d + 16, 0); put16(d + 16, net_csum4(get32(f + 26), get32(f + 30), 6, d, dn)); }
    } else if (type == 0x86DD && n >= 54 && (f[14] >> 4) == 6) {
        uint32_t pl = get16(f + 18);
        if (54 + pl > n) return;
        uint8_t proto, nh = f[20]; uint32_t off;
        if (ip6_walk(f + 54, pl, nh, &proto, &off) != 0) return;
        uint8_t* d = f + 54 + off; uint32_t dn = pl - off;
        if (proto == 58 && dn >= 4) { put16(d + 2, 0); put16(d + 2, net_csum6(f + 22, f + 38, 58, d, dn)); }
        else if (proto == 17 && dn >= 8) { put16(d + 6, 0); uint16_t c = net_csum6(f + 22, f + 38, 17, d, dn); put16(d + 6, c ? c : 0xFFFF); }
        else if (proto == 6 && dn >= 20) { put16(d + 16, 0); put16(d + 16, net_csum6(f + 22, f + 38, 6, d, dn)); }
    }
}
/* Обрезать кадр до n байт и подогнать длины заголовков под новый размер (IPv4 общая длина, IPv6 длина данных, длина UDP):
   так обрезанное доходит до разбора содержимого (опции, имена, поля DHCP), а не отбрасывается по длине заголовка. */
static void trunc_fit(uint8_t* f, uint32_t n) {
    if (n < 34) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0800 && (f[14] >> 4) == 4) {
        uint32_t ihl = (uint32_t)(f[14] & 15) * 4;
        put16(f + 16, (uint16_t)(n - 14));
        if (f[23] == 17 && ihl >= 20 && 14 + ihl + 8 <= n && !(get16(f + 20) & 0x3FFF)) put16(f + 14 + ihl + 4, (uint16_t)(n - 14 - ihl));
    } else if (type == 0x86DD && n >= 54 && (f[14] >> 4) == 6) {
        put16(f + 18, (uint16_t)(n - 54));
        if (f[20] == 17 && n >= 62) put16(f + 58, (uint16_t)(n - 54));
    }
}
static uint32_t mutate(uint8_t* f, uint32_t n) {
    int force_fix = 0;
    int ops = 1 + (int)rr(3);
    while (ops--) {
        switch (rr(10)) {
        case 0: if (n) f[pick_off(n)] = (uint8_t)rnd(); break;
        case 1: if (n) f[pick_off(n)] ^= (uint8_t)(1u << rr(8)); break;
        case 2: { uint32_t r = rr(5); n = r == 0 ? rr(n + 1) : r == 1 ? (n ? n - 1 : 0) : r == 2 ? (n > 14 ? 14 : n) : r == 3 ? (n > 34 ? 34 : n) : (n > 54 ? 54 : n); break; }
        case 3: { uint32_t add = 1 + rr(40); for (uint32_t i = 0; i < add && n < 2048; i++) f[n++] = chance(50) ? 0 : (uint8_t)rnd(); break; }
        case 4: case 5: {
            if (!n) break;
            uint32_t o = pick_off(n), v = EDGE[rr(sizeof EDGE / sizeof EDGE[0])];
            if (chance(15)) v = n - 14; else if (chance(10)) v = n - 34;
            uint32_t w = 1u << rr(3);                           /* 1, 2 или 4 байта */
            for (uint32_t i = 0; i < w && o + i < n; i++) f[o + i] = (uint8_t)(v >> (8 * (w - 1 - i)));
            break;
        }
        case 6: if (n > 4) { uint32_t a = rr(n), b = rr(n), l = 1 + rr(16); for (uint32_t i = 0; i < l && a + i < n && b + i < n; i++) f[b + i] = f[a + i]; } break;
        case 7: if (n > 20) { uint32_t o = pick_off(n); f[o] = (uint8_t)(f[o] + (chance(50) ? 1 : 255)); } break;
        default: if (n > 40) { n = 14 + rr(n - 14); if (chance(50)) n = pick_off(n) + rr(3); if (n < 14) n = 14; trunc_fit(f, n); force_fix = 1; } break;   /* обрезка с подгонкой длин */
        }
    }
    if (force_fix || chance(60)) fix_sums(f, n);
    return n;
}

/* ---------- подача кадров ---------- */
static void check_state(void) {
    for (int i = 0; i < TCP_MAX; i++) {
        const tcb_t* t = &tcbs[i];
        if (t->state < T_FREE || t->state > T_CLOSED) BAD("tcb %d: состояние %d", i, t->state);
        if (t->state == T_FREE) continue;
        if (t->rx_tail - t->rx_head > TCP_RX) BAD("tcb %d: кольцо приёма занято %u", i, t->rx_tail - t->rx_head);
        if (t->tx_len > TCP_TX) BAD("tcb %d: tx_len %u", i, t->tx_len);
        if (t->mss > 1460) BAD("tcb %d: MSS %u", i, t->mss);
    }
    for (int i = 0; i < UDP_MAX; i++) if (us[i].owner && us[i].tail - us[i].head > UDP_RING) BAD("udp %d: кольцо занято %u", i, us[i].tail - us[i].head);
    for (int i = 0; i < IPF_SLOTS; i++) {
        const ipf_slot_t* s = &frag.slot[i];
        if (!s->used) continue;
        if (s->blocks > IPF_BLOCKS || s->maxend > IPF_MAX || (s->total && s->total > IPF_MAX)) BAD("ipfrag: слот %d blocks=%u maxend=%u total=%u", i, s->blocks, s->maxend, s->total);
    }
    for (int i = 0; i < A6_MAX; i++) if (a6[i].state > A6_PREF) BAD("a6 %d: состояние %d", i, a6[i].state);
    if (mtu6 < 1280 || mtu6 > 1500) BAD("mtu6 = %u", mtu6);
    if (dstate < D_INIT || dstate > D_BOUND) BAD("dstate %d", dstate);
}
static void inject(const uint8_t* f, uint32_t n) {
    if (n > 2048) n = 2048;
    uint32_t k = (uint32_t)(g_hist_n % HIST);
    memcpy(g_hist[k], f, n); g_histn[k] = n; g_hist_n++;
    g_inputs++;
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t rx0 = rx_frames, dr0 = rx_drop;
    uint8_t* exact = (uint8_t*)malloc(n ? n : 1);                  /* кадр ровно в своих n байтах: чтение за ним ASan видит, как и использование после возврата */
    memcpy(exact, f, n);
    net_rx_frame(exact, n);
    free(exact);
    clock_gettime(CLOCK_MONOTONIC, &b);
    if ((b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec) > 1000000000LL) BAD("кадр обрабатывался дольше секунды");
    if (rx_frames != rx0 + 1 || (n < 14 && rx_drop != dr0 + 1)) BAD("счётчики приёма: rx %llu->%llu drop %llu->%llu при n=%u", (unsigned long long)rx0, (unsigned long long)rx_frames, (unsigned long long)dr0, (unsigned long long)rx_drop, n);
    check_state();
}
static void inject_mut(const uint8_t* f, uint32_t n, int pct) {
    uint8_t b[2048];
    memcpy(b, f, n);
    if (chance((uint32_t)pct)) n = mutate(b, n);
    inject(b, n);
}

/* ---------- состояние стека ---------- */
static int g_sock4 = -1, g_sock6 = -1, g_l4 = -1, g_l6 = -1;
static void setup_net_as(int bound4, int v6);
static void setup_net(void) { int b = chance(70); int v = chance(20) ? 1 : chance(80) ? 2 : 0; setup_net_as(b, v); }
/* v6: 0 - IPv6 выключен, 1 - только поднят (адрес канала пробный), 2 - адреса рабочие после RA */
static void setup_net_as(int bound4, int v6) {
    reset_all();
    g_ticks = 100000 + rr(1000);
    ip6_linklocal(g_mac, g_ll);
    ip6_slaac(PFX6, 64, g_mac, g_gl);
    g_sock4 = g_sock6 = g_l4 = g_l6 = -1;
    if (bound4) {
        dstate = D_BOUND; my_ip = MY_IP; my_mask = 0xFFFFFF00u; my_gw = PEER_IP; my_dns = DNS_IP; lease_s = 3600; dbound_at = g_ticks; g_bound4 = 1;
        arp_learn(PEER_IP, PEER_MAC); arp_learn(DNS_IP, PEER_MAC);
        dns_port = 49999; dns_id = 0x1234; ping_ip = PEER_IP; ping_seq = 7;
        g_sock4 = (int)net_udp_sys(NET_UDP_OPEN, 5000, 0, 0);
        g_l4 = tcp_listen(80); tcp_listen(8080);
    } else {
        static const int st[] = { D_INIT, D_SELECTING, D_REQUESTING };
        dstate = st[rr(3)]; xid = 0x11223344u; offer_ip = MY_IP; offer_srv = PEER_IP; dns_port = 49999; dns_id = 0x1234;
    }
    if (v6 == 1) {                                             /* IPv6 только поднят: адрес канала ещё «пробный» (DAD идёт) */
        v6_up(g_ticks);
        if (chance(50)) { g_ticks += TIMER_HZ + 3; v6_tick(g_ticks); }
    } else if (v6 == 2) {
        v6_up(g_ticks);
        for (int i = 0; i < 4; i++) { g_ticks += TIMER_HZ + 3; v6_tick(g_ticks); }
        uint8_t ra[420], b[460], f[2048];
        uint32_t m = ra_pkt(ra, PEER6, g_ll); (void)m;
        memset(ra, 0, sizeof ra);                              /* хороший RA: маршрутизатор, префикс /64 A+L, MTU, DNS */
        ra[0] = 134; put16(ra + 6, 1800); ra[16] = 1; ra[17] = 1; cpy(ra + 18, PEER_MAC, 6);
        ra[24] = 3; ra[25] = 4; ra[26] = 64; ra[27] = 0xC0; put32(ra + 28, 86400); put32(ra + 32, 14400); cpy(ra + 40, PFX6, 16);
        ra[56] = 5; ra[57] = 1; put32(ra + 60, 1500); ra[64] = 25; ra[65] = 3; put32(ra + 68, 600); cpy(ra + 72, FAR6, 16);
        put16(ra + 2, net_csum6(PEER6, g_ll, 58, ra, 88));
        uint32_t bn = ip6_build(b, PEER6, g_ll, 58, 255, ra, 88);
        uint32_t fn = eth_wrap(f, 0x86DD, b, bn);
        inject(f, fn);
        for (int i = 0; i < 4; i++) { g_ticks += TIMER_HZ + 3; v6_tick(g_ticks); }
        nd_learn(PEER6, PEER_MAC); nd_learn(FAR6, PEER_MAC);
        g_v6ready = a6_state(g_gl) == A6_PREF && a6_state(g_ll) == A6_PREF;
        if (!g_v6ready) BAD("после RA адреса IPv6 не стали рабочими (ll %d, gl %d)", a6_state(g_ll), a6_state(g_gl));
        g_sock6 = (int)net_udp_sys(NET_UDP_OPEN, 5000 | 6u << 16, 0, 0);
        g_l6 = tcp_listen6(80);
        cpy(ping6_ip, PEER6, 16); ping6_seq = 9;
    }
    g_nout = 0;
}

/* ---------- образцы ---------- */
static void gen_kind(int kind) {
    g_nin = 0;
    uint8_t pl[2100], b[2100], d[1600];
    ipa_t s4 = a4(pick4()), d4 = a4(pick4dst()), s6 = a6x(pick6()), d6 = a6x(pick6dst());
    uint32_t n, m;
    uint8_t data[2000]; for (int i = 0; i < 2000; i++) data[i] = (uint8_t)rnd();
    uint32_t dl = chance(70) ? rr(64) : chance(85) ? rr(1473) : 1400 + rr(550);      /* до размера буфера приёма (2048): кадры больше MTU тоже приходят */
    uint16_t dport = chance(60) ? 5000 : (uint16_t)rr(65536);
    switch (kind) {
    case 0: case 1: case 2:                                   /* ARP: запрос нас, ответ, чужой запрос */
        put16(pl, 1); put16(pl + 2, 0x0800); pl[4] = 6; pl[5] = 4; put16(pl + 6, kind == 1 ? 2 : 1);
        cpy(pl + 8, PEER_MAC, 6); put32(pl + 14, s4.a[0] ? ipa_to4(&s4) : PEER_IP); cpy(pl + 18, g_mac, 6); put32(pl + 24, kind == 2 ? FAR_IP : my_ip ? my_ip : MY_IP);
        donef(eth_wrap(newf(), 0x0806, pl, 28)); break;
    case 3: case 4:                                            /* ICMP эхо-запрос / эхо-ответ */
        n = 8 + dl; pl[0] = kind == 3 ? 8 : 0; pl[1] = 0; put16(pl + 2, 0); put16(pl + 4, chance(70) ? 0x4B49 : (uint16_t)rnd()); put16(pl + 6, chance(70) ? 7 : (uint16_t)rnd());
        cpy(pl + 8, data, dl); put16(pl + 2, net_csum(pl, n));
        donef(l3_frame(newf(), &s4, &d4, 1, pl, n)); break;
    case 5: {                                                  /* большое эхо фрагментами IPv4 */
        n = 8 + 1 + rr(8190 - 8); pl[0] = 0; (void)pl;
        static uint8_t big[8200]; for (uint32_t i = 0; i < n; i++) big[i] = (uint8_t)rnd();
        big[0] = 8; big[1] = 0; put16(big + 2, 0); put16(big + 4, 0x4B49); put16(big + 6, 7); put16(big + 2, net_csum(big, n));
        frag4(ipa_to4(&s4), ipa_to4(&d4), 1, big, n, rr(2), rr(2), rr(4) == 0); break; }
    case 6: case 7: case 8:                                    /* UDP на наш порт (с суммой, без суммы, на широковещательный) */
        m = udp_build(pl, &s4, &d4, (uint16_t)(1024 + rr(60000)), dport, data, dl, kind != 7);
        donef(l3_frame(newf(), &s4, &d4, 17, pl, m)); break;
    case 9: case 10: case 11: {                                /* DHCP OFFER / ACK / NAK от сервера */
        uint32_t q = dhcp_pkt(d, 2, chance(85) ? xid : rnd(), chance(80) ? MY_IP : rnd(), kind == 9 ? 2 : kind == 10 ? 5 : 6);
        ipa_t sv = a4(PEER_IP), bc = a4(chance(50) ? 0xFFFFFFFFu : MY_IP);
        m = udp_build(pl, &sv, &bc, 67, 68, d, q, chance(50));
        donef(l3_frame(newf(), &sv, &bc, 17, pl, m)); break; }
    case 12: {                                                 /* DNS-ответ (на порт, что ждёт стек) */
        uint32_t q = dns_pkt(d, chance(90) ? dns_id : (uint16_t)rnd(), (int)rr(5));
        ipa_t sv = a4(chance(90) ? DNS_IP : PEER_IP), me = a4(MY_IP);
        m = udp_build(pl, &sv, &me, chance(90) ? 53 : 5353, chance(90) ? dns_port : 5000, d, q, 0);
        donef(l3_frame(newf(), &sv, &me, 17, pl, m)); break; }
    case 13: case 14: case 15: case 16: case 17: {             /* TCP: SYN на слушаемый/закрытый порт, данные, FIN, RST */
        static const uint8_t fls[] = { F_SYN, F_SYN, F_ACK, F_ACK | F_PSH, F_FIN | F_ACK, F_RST, F_RST | F_ACK, F_SYN | F_ACK, F_SYN | F_FIN, 0 };
        uint8_t fl = kind == 13 ? F_SYN : kind == 16 ? (F_FIN | F_ACK) : kind == 17 ? F_RST : fls[rr(sizeof fls)];
        uint8_t opt[48]; uint32_t on = tcp_opts(opt);
        const ipa_t* ss = chance(50) ? &s4 : &s6; const ipa_t* dd = ss == &s4 ? &d4 : &d6;
        m = tcp_build(pl, ss, dd, (uint16_t)(40000 + rr(3)), chance(70) ? 80 : (uint16_t)(chance(50) ? 8080 : rnd()), chance(50) ? 1000 : rnd(), chance(50) ? 0 : rnd(), fl, (uint16_t)(chance(50) ? 65535 : rnd()), opt, on, data, chance(60) ? 0 : dl);
        donef(l3_frame(newf(), ss, dd, 6, pl, m)); break; }
    case 18:                                                   /* ND: NS (запрос соседа) нам, DAD, NA */
    case 19:
    case 20: {
        const uint8_t* tg = chance(50) ? g_ll : g_gl;
        uint8_t un[16] = { 0 }, sn[16];
        ip6_solicited(tg, sn);
        const uint8_t* src = kind == 19 && chance(60) ? un : PEER6;
        const uint8_t* dst = kind == 20 ? g_ll : (chance(85) ? sn : g_ll);
        n = kind == 20 ? nd_build_na(pl, src, dst, chance(80) ? tg : FAR6, (uint8_t)(chance(50) ? ND_NA_S | ND_NA_O : rnd()), chance(70) ? PEER_MAC : 0)
                       : nd_build_ns(pl, src, dst, tg, chance(70) && src != un ? PEER_MAC : 0);
        donef(eth_wrap(newf(), 0x86DD, b, ip6_build(b, src, dst, 58, chance(90) ? 255 : (uint8_t)rnd(), pl, n))); break; }
    case 21: case 22: {                                        /* RA от маршрутизатора (случайные опции) */
        n = ra_pkt(pl, PEER6, g_ll);
        if (kind == 22) { put16(pl + 6, 0); put16(pl + 2, 0); put16(pl + 2, net_csum6(PEER6, g_ll, 58, pl, n)); }
        donef(eth_wrap(newf(), 0x86DD, b, ip6_build(b, PEER6, g_ll, 58, chance(90) ? 255 : (uint8_t)rnd(), pl, n))); break; }
    case 23: case 24: {                                        /* ICMPv6 эхо: запрос / ответ / прочее */
        uint8_t t = kind == 23 ? 128 : chance(50) ? 129 : (uint8_t)rnd();
        n = icmp6_build_echo(pl, s6.a, d6.a, t, chance(70) ? 0x4B49 : (uint16_t)rnd(), chance(70) ? 9 : (uint16_t)rnd(), data, dl);
        donef(eth_wrap(newf(), 0x86DD, b, ip6_build(b, s6.a, d6.a, 58, 64, pl, n))); break; }
    case 25: case 26:                                          /* UDP и TCP по IPv6, в том числе с заголовками расширения */
        if (kind == 25) { m = udp_build(pl, &s6, &d6, 4000, dport, data, dl, 1); }
        else { uint8_t opt[48]; uint32_t on = tcp_opts(opt); m = tcp_build(pl, &s6, &d6, 40001, 80, 1000, 0, F_SYN, 65535, opt, on, data, chance(70) ? 0 : dl); }
        {
            uint8_t proto = kind == 25 ? 17 : 6, ex[200], types[4]; uint32_t en = 0, hops = chance(50) ? 1 + rr(3) : 0;
            static const uint8_t kinds[] = { 0, 60, 43, 51 };      /* параметры, получатель, маршрутизация, AH (последний отвергается) */
            for (uint32_t h = 0; h < hops; h++) types[h] = kinds[rr(chance(85) ? 3 : 4)];
            for (uint32_t h = 0; h < hops; h++) {
                uint8_t units = (uint8_t)(chance(70) ? 0 : rr(3)); uint32_t sz = ((uint32_t)units + 1) * 8;
                memset(ex + en, 0, sz); ex[en] = h + 1 < hops ? types[h + 1] : proto; ex[en + 1] = units;
                if (types[h] == 43) ex[en + 3] = (uint8_t)(chance(80) ? 0 : 1);   /* остаток узлов маршрута */
                en += sz;
            }
            uint8_t body[2100]; cpy(body, ex, en); cpy(body + en, pl, m);
            donef(eth_wrap(newf(), 0x86DD, b, ip6_build(b, s6.a, d6.a, hops ? types[0] : proto, 64, body, en + m)));
            if (chance(80)) fix_sums(g_in[g_nin - 1], g_inlen[g_nin - 1]);
        }
        break;
    case 27: {                                                 /* фрагменты IPv6 с UDP внутри */
        n = 1 + rr(8100); static uint8_t big[8200];
        for (uint32_t i = 0; i < n; i++) big[i] = (uint8_t)rnd();
        ipa_t s = a6x(PEER6), dd = a6x(chance(50) ? g_gl : g_ll);
        m = udp_build(big, &s, &dd, 4000, 5000, big + 8, n > 8 ? n - 8 : 0, 1);
        frag6(s.a, dd.a, 17, big, m, rr(2), rr(2), rr(4) == 0); break; }
    case 28: {                                                 /* фрагменты IPv4 с UDP внутри */
        n = 1 + rr(8180); static uint8_t big[8200];
        for (uint32_t i = 0; i < n; i++) big[i] = (uint8_t)rnd();
        ipa_t s = a4(PEER_IP), dd = a4(MY_IP);
        m = udp_build(big, &s, &dd, 4000, 5000, big + 8, n, 1);
        frag4(PEER_IP, MY_IP, 17, big, m, rr(2), rr(2), rr(4) == 0); break; }
    case 29: {                                                 /* случайный транспорт внутри верного IPv4/IPv6 */
        n = rr(300); for (uint32_t i = 0; i < n; i++) pl[i] = (uint8_t)rnd();
        static const uint8_t pr[] = { 1, 6, 17, 58, 2, 0, 44, 255 };
        if (chance(50)) { m = ip4_build(b, pick4(), pick4dst(), pr[rr(sizeof pr)], (uint16_t)rnd(), chance(80) ? 0x4000 : (uint16_t)rnd(), pl, n); donef(eth_wrap(newf(), 0x0800, b, m)); }
        else { m = ip6_build(b, pick6(), pick6dst(), pr[rr(sizeof pr)], 64, pl, n); donef(eth_wrap(newf(), 0x86DD, b, m)); }
        if (chance(70)) fix_sums(g_in[0], g_inlen[0]);
        break; }
    case 30: default: {                                        /* чистая случайность: любая длина, любые байты */
        n = rr(chance(10) ? 2048 : 200); uint8_t* f = newf(); for (uint32_t i = 0; i < n; i++) f[i] = (uint8_t)rnd();
        if (chance(60) && n >= 14) put16(f + 12, chance(34) ? 0x0806 : chance(50) ? 0x0800 : 0x86DD);
        if (chance(40) && n >= 15) f[14] = chance(50) ? 0x45 : 0x60;
        donef(n); break; }
    }
    (void)d;
}
#define NKINDS 31

/* ---------- чтение сокетов и отправка ---------- */
static void drain(void) {
    int socks[2] = { g_sock4, g_sock6 };
    for (int k = 0; k < 2; k++) {
        if (socks[k] < UH) continue;
        for (int guard = 0; guard < 8 && us[socks[k] - UH].tail != us[socks[k] - UH].head; guard++) {
            uint32_t cap = chance(30) ? 24 + rr(300) : 24 + UDP_DATA;
            uint8_t* out = (uint8_t*)malloc(cap + 8);
            int64_t r = net_udp_sys(NET_UDP_RECVFROM, (uint64_t)socks[k], (uint64_t)(uintptr_t)out, cap | 1ull << 32);
            if (r < 0 || (uint64_t)r > cap - 24) BAD("udp RECVFROM вернул %lld при ёмкости %u", (long long)r, cap);
            free(out);
        }
    }
}
static void udp_send_some(void) {
    uint8_t data[8400]; for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)rnd();
    uint32_t n = chance(40) ? rr(100) : chance(50) ? EDGE[rr(sizeof EDGE / sizeof EDGE[0])] % 9000 : 1470 + rr(10);
    if (g_sock4 >= UH && chance(60)) {
        int64_t r = net_udp_sys(NET_UDP_SEND, (uint64_t)g_sock4 | (uint64_t)(rr(65535) + 1) << 16 | (uint64_t)(chance(50) ? PEER_IP : FAR_IP) << 32, (uint64_t)(uintptr_t)data, n);
        if (r > (int64_t)n) BAD("udp SEND вернул %lld > %u", (long long)r, n);
    } else if (g_sock6 >= UH) {
        ksockaddr_t sa; memset(&sa, 0, sizeof sa); sa.family = 6; sa.port = (uint16_t)(rr(65535) + 1); cpy(sa.addr, chance(50) ? PEER6 : FAR6, 16);
        int64_t r = net_udp_sys(NET_UDP_SENDTO, (uint64_t)g_sock6 | (uint64_t)n << 16, (uint64_t)(uintptr_t)&sa, (uint64_t)(uintptr_t)data);
        if (r > (int64_t)n) BAD("udp SENDTO вернул %lld > %u", (long long)r, n);
    }
}

/* ---------- сценарии ---------- */
static void scen_frames(int count) {
    for (int i = 0; i < count; i++) {
        int kind = (int)rr(NKINDS);
        gen_kind(kind);
        for (unsigned j = 0; j < g_nin; j++) {
            if (g_inlen[j] > 2048) continue;
            inject_mut(g_in[j], g_inlen[j], kind == 30 ? 0 : chance(25) ? 80 : 35);
            if (chance(12)) { g_ticks += rr(TIMER_HZ * 3); host_tick(); }
        }
        if (chance(30)) drain();
        if (chance(8)) udp_send_some();
        if (chance(3)) { g_ticks += TIMER_HZ * (1 + rr(40)); host_tick(); }
    }
    drain();
}

static void scen_dhcp(void) {
    g_bound4 = 0; my_ip = 0; dstate = D_INIT; dnext = 0; dtries = 0;
    g_ticks += 10; host_tick();                                  /* DISCOVER */
    for (int round = 0; round < 3 + (int)rr(4); round++) {
        for (int k = 0; k < 2; k++) {
            uint8_t d[600], pl[700], f[2048];
            int mt = k == 0 ? 2 : (chance(75) ? 5 : 6);
            uint32_t q = dhcp_pkt(d, 2, chance(85) ? xid : rnd(), chance(85) ? MY_IP : rnd(), chance(90) ? mt : (int)rr(10));
            ipa_t sv = a4(PEER_IP), bc = a4(chance(50) ? 0xFFFFFFFFu : MY_IP);
            uint32_t m = udp_build(pl, &sv, &bc, 67, 68, d, q, chance(30));
            uint32_t n = l3_frame(f, &sv, &bc, 17, pl, m);
            inject_mut(f, n, 30);
            g_ticks += rr(TIMER_HZ * 3); host_tick();
        }
        if (dstate == D_BOUND && (my_mask == 0 && my_ip != 0)) BAD("DHCP: адрес есть, маски нет");
        if (chance(30)) { g_ticks += TIMER_HZ * 1800 * (1 + rr(2)); host_tick(); }   /* продление аренды */
    }
    scen_frames(4);
}

/* «Собеседник» по TCP: отвечает на то, что стек отправил, пока тот ждёт (зовётся из сна). */
typedef struct { int fam; ipa_t ip; uint16_t port; uint16_t lport; uint32_t snd_nxt, ack; int fin_sent, active; uint32_t isn; int mut; } peer_t;
static peer_t g_peer;
static uint64_t g_seen;
static int g_in_hook;
static int g_hk_dns;                                            /* 1 - отвечает на запросы DNS */

static void peer_seg(uint32_t seq, uint32_t ack, uint8_t fl, const uint8_t* data, uint32_t n) {
    uint8_t pl[1600], f[2048]; ipa_t me = g_peer.fam == 4 ? a4(my_ip) : a6x(g_gl);
    uint8_t opt[4] = { 2, 4, 0x05, 0xB4 };
    uint32_t m = tcp_build(pl, &g_peer.ip, &me, g_peer.port, g_peer.lport, seq, ack, fl, 65535, (fl & F_SYN) ? opt : 0, (fl & F_SYN) ? 4 : 0, data, n);
    if (g_peer.fam == 6) { ipa_t dd = a6x(g_ll); (void)dd; }
    uint32_t fn = l3_frame(f, &g_peer.ip, &me, 6, pl, m);
    inject_mut(f, fn, g_peer.mut);
}
static void peer_react(void) {
    unsigned guard = 0;
    if (g_nout > g_seen + OUT_N) g_seen = g_nout - OUT_N;
    while (g_seen < g_nout && guard++ < 24) {
        uint32_t k = (uint32_t)(g_seen % OUT_N); g_seen++;
        const uint8_t* f = g_out[k]; uint32_t n = g_outn[k];
        if (n < 54) continue;
        const uint8_t *tp; uint32_t tn; ipa_t dst;
        if (get16(f + 12) == 0x0800 && (get16(f + 20) & 0x3FFF) == 0) { tp = f + 34; tn = n - 34; if (f[23] != 6) continue; dst = a4(get32(f + 30)); }
        else if (get16(f + 12) == 0x86DD && f[20] == 6) { tp = f + 54; tn = n - 54; dst = a6x(f + 38); }
        else continue;
        if (!g_peer.active || !ipa_eq(&dst, &g_peer.ip) || tn < 20) continue;
        uint32_t hl = (uint32_t)(tp[12] >> 4) * 4; if (hl < 20 || hl > tn) continue;
        if (get16(tp + 2) != g_peer.port) continue;
        uint8_t fl = tp[13]; uint32_t seq = get32(tp + 4), dl = tn - hl;
        g_peer.lport = get16(tp);
        if (fl & F_RST) continue;
        uint32_t next = seq + dl + ((fl & F_SYN) ? 1 : 0) + ((fl & F_FIN) ? 1 : 0);
        if ((fl & F_SYN) && !(fl & F_ACK)) { g_peer.snd_nxt = g_peer.isn; peer_seg(g_peer.snd_nxt, next, F_SYN | F_ACK, 0, 0); g_peer.snd_nxt++; g_peer.ack = next; }
        else if (fl & F_SYN) { g_peer.ack = next; peer_seg(g_peer.snd_nxt, next, F_ACK, 0, 0); }
        else if (dl || (fl & F_FIN)) {
            g_peer.ack = next; peer_seg(g_peer.snd_nxt, next, F_ACK, 0, 0);
            if ((fl & F_FIN) && !g_peer.fin_sent) { g_peer.fin_sent = 1; peer_seg(g_peer.snd_nxt, next, F_FIN | F_ACK, 0, 0); g_peer.snd_nxt++; }
        }
    }
}
static void dns_react(void) {
    unsigned guard = 0;
    if (g_nout > g_seen + OUT_N) g_seen = g_nout - OUT_N;
    while (g_seen < g_nout && guard++ < 8) {
        uint32_t k = (uint32_t)(g_seen % OUT_N); g_seen++;
        const uint8_t* f = g_out[k]; uint32_t n = g_outn[k];
        if (n < 42 || get16(f + 12) != 0x0800 || f[23] != 17 || get16(f + 36) != 53) continue;
        uint16_t id = get16(f + 42), sport = get16(f + 34);
        uint8_t d[700], pl[800], fr[2048];
        uint32_t q = dns_pkt(d, chance(90) ? id : (uint16_t)rnd(), (int)rr(6));
        ipa_t sv = a4(DNS_IP), me = a4(my_ip);
        uint32_t m = udp_build(pl, &sv, &me, 53, chance(92) ? sport : (uint16_t)rnd(), d, q, chance(30));
        uint32_t fn = l3_frame(fr, &sv, &me, 17, pl, m);
        ASAN_UNPOISON_MEMORY_REGION(dns_buf, sizeof dns_buf);
        inject_mut(fr, fn, 40);
        if (dns_got && dns_len < sizeof dns_buf) ASAN_POISON_MEMORY_REGION(dns_buf + dns_len, sizeof dns_buf - dns_len);   /* хвост буфера за принятым - ловушка: разбор ответа за длину ответа виден */
    }
}
static void hook(void) {
    if (g_in_hook) return;
    g_in_hook = 1;
    host_tick();
    if (g_hk_dns) dns_react(); else peer_react();
    g_in_hook = 0;
}

static void scen_dns(void) {
    if (!g_bound4) return;
    g_hook = hook; g_hk_dns = 1; g_seen = g_nout;
    for (int i = 0; i < 1 + (int)rr(3); i++) {
        uint32_t ip = 0xAAAAAAAAu;
        static const char* names[] = { "example.com", "a.b.c.d.e.f", "x", "10.1.2.3", "www.example.org" };
        int r = net_dns(names[rr(5)], &ip);
        if (r != 0 && r != -4 && r != -5 && r != -6 && r != -1 && r != -2 && r != -3) BAD("net_dns вернул %d", r);
        if (r == 0 && ip == 0xAAAAAAAAu) BAD("net_dns вернул 0, адрес не записан");
    }
    g_hook = 0; g_hk_dns = 0;
}

static void scen_tcpflow(void) {
    int v6 = g_v6ready && (!g_bound4 || chance(50));
    if (!v6 && !g_bound4) return;
    memset(&g_peer, 0, sizeof g_peer);
    g_peer.fam = v6 ? 6 : 4; g_peer.ip = v6 ? a6x(chance(50) ? PEER6 : FAR6) : a4(chance(50) ? PEER_IP : FAR_IP);
    g_peer.isn = rnd(); g_peer.active = 1; g_peer.mut = chance(70) ? 4 : 30;
    g_hook = hook; g_hk_dns = 0; g_seen = g_nout;
    int h = -1, client = chance(50);
    if (client) {
        g_peer.port = 80;
        h = v6 ? tcp_connect6(g_peer.ip.a, 80) : tcp_connect(ipa_to4(&g_peer.ip), 80);
    } else {
        g_peer.port = (uint16_t)(40000 + rr(3)); g_peer.lport = 80;
        ipa_t me = v6 ? a6x(g_gl) : a4(my_ip);
        uint8_t pl[100], f[2048], opt[4] = { 2, 4, 0x05, 0xB4 };
        uint32_t m = tcp_build(pl, &g_peer.ip, &me, g_peer.port, 80, g_peer.isn, 0, F_SYN, 65535, opt, 4, 0, 0);
        uint32_t fn = l3_frame(f, &g_peer.ip, &me, 6, pl, m);
        g_peer.snd_nxt = g_peer.isn + 1;
        inject_mut(f, fn, 10);
        h = tcp_accept(v6 ? g_l6 : g_l4, (uint32_t)rr(4));
    }
    int ops = 3 + (int)rr(6);
    for (int i = 0; i < ops; i++) {
        uint8_t data[18600], buf[300];
        for (unsigned k = 0; k < sizeof data; k++) data[k] = (uint8_t)rnd();
        switch (rr(8)) {
        case 0: if (h >= 0) { uint32_t n = chance(50) ? rr(200) : rr(6000); int r = tcp_send(h, data, n); if (r > (int)n) BAD("tcp_send вернул %d > %u", r, n); } break;
        case 1: if (h >= 0) { uint32_t cap = 1 + rr(300); int r = tcp_recv(h, buf, cap, rr(5)); if (r > (int)cap) BAD("tcp_recv вернул %d > %u", r, cap); } break;
        case 2: { uint32_t n = chance(8) ? 16000 + rr(2500) : chance(50) ? rr(100) : rr(3000), off = 0;                      /* данные собеседника, порциями */
                  while (off < n) { uint32_t c = 1 + rr(1460); if (off + c > n) c = n - off; peer_seg(g_peer.snd_nxt, g_peer.ack, F_ACK | F_PSH, data + off, c); g_peer.snd_nxt += c; off += c; } break; }
        case 3: peer_seg(g_peer.snd_nxt, g_peer.ack, F_FIN | F_ACK, 0, 0); g_peer.snd_nxt++; break;
        case 4: peer_seg(g_peer.snd_nxt + (chance(50) ? 0 : rnd() % 100000), g_peer.ack, F_RST | (chance(50) ? F_ACK : 0), 0, 0); break;
        case 5: {                                                                           /* сегмент со значениями вокруг верных */
            static const uint8_t fls[] = { F_ACK, F_ACK | F_PSH, F_FIN | F_ACK, F_SYN, F_SYN | F_ACK, F_RST, F_ACK | F_FIN | F_PSH };
            uint32_t ds = chance(50) ? 0 : rr(300);
            peer_seg(g_peer.snd_nxt + (uint32_t)((int)rr(9) - 4) * (chance(30) ? 0x40000000u : 1), g_peer.ack + (uint32_t)((int)rr(9) - 4) * (chance(30) ? 0x40000000u : 1),
                     fls[rr(sizeof fls)], data, ds); break; }
        case 6: g_ticks += rr(TIMER_HZ * 20); host_tick(); break;
        default: drain(); break;
        }
    }
    if (h >= 0) tcp_close(h);
    if (chance(60)) {                                            /* повтор FIN и случайные сегменты после закрытия (TIME_WAIT, чужая четвёрка) */
        peer_seg(g_peer.snd_nxt - 1, g_peer.ack, F_FIN | F_ACK, 0, 0);
        if (chance(50)) peer_seg(g_peer.snd_nxt, g_peer.ack, F_ACK, 0, 0);
        if (chance(30)) { g_ticks += rr(TIMER_HZ * 70); host_tick(); peer_seg(g_peer.snd_nxt - 1, g_peer.ack, F_FIN | F_ACK, 0, 0); }
    }
    g_hook = 0; g_peer.active = 0;
}

/* Много датаграмм UDP подряд без чтения: кольцо сокета заполняется, лишние отбрасываются (счётчик), принятые целы и по порядку. */
static void scen_udp_flood(void) {
    int use6 = g_v6ready && (!g_bound4 || chance(50));
    if (!use6 && !g_bound4) return;
    int sock = use6 ? g_sock6 : g_sock4;
    ipa_t s = use6 ? a6x(PEER6) : a4(PEER_IP), d = use6 ? a6x(chance(50) ? g_gl : g_ll) : a4(MY_IP);
    unsigned count = 10 + rr(40);
    uint32_t len[64];
    for (unsigned k = 0; k < count; k++) {
        uint8_t data[1500], pl[1600], f[2048];
        len[k] = chance(30) ? rr(40) : 1 + rr(1472);
        for (uint32_t i = 0; i < len[k]; i++) data[i] = (uint8_t)(k * 31 + i);
        uint32_t m = udp_build(pl, &s, &d, 4000, 5000, data, len[k], 1);
        uint32_t n = l3_frame(f, &s, &d, 17, pl, m);
        inject(f, n);
    }
    uint8_t* out = (uint8_t*)malloc(24 + UDP_DATA + 8);
    int last = -1;
    while (us[sock - UH].tail != us[sock - UH].head) {
        int64_t r = net_udp_sys(NET_UDP_RECVFROM, (uint64_t)sock, (uint64_t)(uintptr_t)out, (24 + UDP_DATA) | 1ull << 32);
        if (r < 0 || r > 1472) { BAD("udp flood: RECVFROM вернул %lld", (long long)r); break; }
        int k = -1;
        for (unsigned j = (unsigned)(last + 1); j < count; j++) if (len[j] == (uint32_t)r) { int ok = 1; for (int64_t i = 0; i < r; i++) if (out[24 + i] != (uint8_t)(j * 31 + (uint32_t)i)) { ok = 0; break; } if (ok) { k = (int)j; break; } }
        if (k < 0) { BAD("udp flood: принятая датаграмма (%lld байт) не совпадает ни с одной из ещё не принятых отправленных или нарушен порядок", (long long)r); break; }
        last = k;
    }
    free(out);
}

/* Фрагменты целой датаграммы в любом порядке: она обязана дойти (UDP - в сокет, эхо IPv4 - ответом) или быть отвергнутой, если не помещается. */
static void scen_frag_clean(void) {
    int use6 = g_v6ready && (!g_bound4 || chance(50));
    if (!use6 && !g_bound4) return;
    static uint8_t big[8300], rx[8300];
    int kind = (int)rr(use6 ? 2 : 3);                              /* 0 - UDP, 1 - UDP (граница размера), 2 - эхо IPv4 */
    uint32_t n = kind == 1 ? (chance(50) ? UDP_DATA : UDP_DATA + 1 + rr(8)) : 1 + rr(UDP_DATA);
    for (uint32_t i = 0; i < n + 8; i++) big[i] = (uint8_t)rnd();
    g_nin = 0;
    if (kind == 2) {
        uint32_t en = 8 + rr(8000);
        big[0] = 8; big[1] = 0; put16(big + 2, 0); put16(big + 4, 0x1234); put16(big + 6, 0x4321); put16(big + 2, net_csum(big, en));
        g_dgcount = 0;
        frag4(PEER_IP, MY_IP, 1, big, en, 1, chance(30), 0);
        for (unsigned j = 0; j < g_nin; j++) inject(g_in[j], g_inlen[j]);
        if (g_dgcount != 1 || g_dgproto != 1 || g_dgn != en || g_dg[0] != 0 || memcmp(g_dg + 8, big + 8, en - 8) != 0 || get16(g_dg + 4) != 0x1234 || get16(g_dg + 6) != 0x4321)
            BAD("эхо из фрагментов (%u байт): ответ не получен или искажён (ответов %d, длина %u)", en, g_dgcount, g_dgn);
        return;
    }
    ipa_t s = use6 ? a6x(PEER6) : a4(PEER_IP), d = use6 ? a6x(chance(50) ? g_gl : g_ll) : a4(MY_IP);
    uint32_t m = udp_build(rx, &s, &d, 4000, 5000, big, n, 1);
    int dup = chance(30), shuf = chance(80);
    if (use6) frag6(s.a, d.a, 17, rx, m, shuf, dup, 0); else frag4(ipa_to4(&s), ipa_to4(&d), 17, rx, m, shuf, dup, 0);
    for (unsigned j = 0; j < g_nin; j++) inject(g_in[j], g_inlen[j]);
    int sock = use6 ? g_sock6 : g_sock4;
    uint8_t* out = (uint8_t*)malloc(24 + UDP_DATA + 8);
    int got = 0;
    while (us[sock - UH].tail != us[sock - UH].head) {
        int64_t r = net_udp_sys(NET_UDP_RECVFROM, (uint64_t)sock, (uint64_t)(uintptr_t)out, (24 + UDP_DATA) | 1ull << 32);
        uint16_t sp; memcpy(&sp, out + 2, 2);                      /* ksockaddr_t: порт в порядке хоста */
        if (r != (int64_t)n || memcmp(out + 24, big, n) != 0 || sp != 4000 || out[0] != (use6 ? 6 : 4)) BAD("датаграмма UDP %u байт из фрагментов дошла искажённой (r=%lld)", n, (long long)r);
        got++;
    }
    free(out);
    if (n <= UDP_DATA && got != 1) BAD("датаграмма UDP %u байт из фрагментов (%s) не дошла (получено %d)", n, use6 ? "IPv6" : "IPv4", got);
    if (n > UDP_DATA && got != 0) BAD("датаграмма %u байт больше предела дошла", n);
}

/* ---------- регрессионные входы: найденные фаззером ошибки ---------- */
#define EXPECT(c, ...) do { if (!(c)) BAD(__VA_ARGS__); } while (0)
static int tcb_live(void) { int n = 0; for (int i = 0; i < TCP_MAX; i++) n += tcbs[i].state != T_FREE; return n; }
static void tcp_frame(uint32_t src, uint32_t dst, uint16_t dport, uint8_t fl, uint32_t seq, uint32_t ack) {
    uint8_t pl[64], f[2048]; ipa_t s = a4(src), d = a4(dst);
    uint32_t m = tcp_build(pl, &s, &d, 40000, dport, seq, ack, fl, 65535, 0, 0, 0, 0);
    inject(f, l3_frame(f, &s, &d, 6, pl, m));
}
static void echo_frame(uint32_t src, uint32_t dst) {
    uint8_t pl[16], f[2048]; ipa_t s = a4(src), d = a4(dst);
    pl[0] = 8; pl[1] = 0; put16(pl + 2, 0); put16(pl + 4, 1); put16(pl + 6, 1); memcpy(pl + 8, "abcdefgh", 8); put16(pl + 2, net_csum(pl, 16));
    inject(f, l3_frame(f, &s, &d, 1, pl, 16));
}
static uint32_t dhcp_fixed(uint8_t* o, uint32_t yi, int mt, uint32_t mask, uint32_t gw) {
    memset(o, 0, 400);
    o[0] = 2; o[1] = 1; o[2] = 6; put32(o + 4, xid); put32(o + 16, yi); cpy(o + 28, g_mac, 6); put32(o + 236, 0x63825363u);
    uint8_t* p = o + 240;
    *p++ = 53; *p++ = 1; *p++ = (uint8_t)mt;
    if (mask) { *p++ = 1; *p++ = 4; put32(p, mask); p += 4; }
    if (gw) { *p++ = 3; *p++ = 4; put32(p, gw); p += 4; }
    *p++ = 54; *p++ = 4; put32(p, PEER_IP); p += 4;
    *p++ = 255;
    return (uint32_t)(p - o);
}
static void dhcp_frame(uint32_t yi, int mt, uint32_t mask, uint32_t gw) {
    uint8_t d[400], pl[500], f[2048]; ipa_t sv = a4(PEER_IP), bc = a4(0xFFFFFFFFu);
    uint32_t q = dhcp_fixed(d, yi, mt, mask, gw), m = udp_build(pl, &sv, &bc, 67, 68, d, q, 0);
    inject(f, l3_frame(f, &sv, &bc, 17, pl, m));
}
static void regress(void) {
    g_scen = (uint64_t)-1; rs = 1;
    /* 1. TCP и эхо - только узлам с одиночными адресами (раньше: RST и SYN-ACK уходили на 0.0.0.0, широковещательный и групповой адрес,
       с широковещательного адреса отправителем; эхо отвечало на 0.0.0.0) */
    setup_net_as(1, 0);
    static const uint32_t bad_src[] = { 0, 0xFFFFFFFFu, 0xE0000001u, 0x7F000001u, 0x0A0002FFu, 0xF0000001u };
    for (unsigned i = 0; i < sizeof bad_src / sizeof bad_src[0]; i++) {
        uint64_t mark = g_nout; int live = tcb_live();
        tcp_frame(bad_src[i], MY_IP, 80, F_SYN, 1000, 0);                 /* на слушаемый порт */
        tcp_frame(bad_src[i], MY_IP, 81, F_SYN | F_ACK, 1000, 5);        /* на закрытый порт: RST не нужен */
        echo_frame(bad_src[i], MY_IP);
        EXPECT(g_nout == mark && tcb_live() == live, "ответ узлу %08x без одиночного адреса: отправлено %llu, соединений %d->%d", bad_src[i], (unsigned long long)(g_nout - mark), live, tcb_live());
    }
    static const uint32_t bad_dst[] = { 0xFFFFFFFFu, 0x0A0002FFu };
    for (unsigned i = 0; i < 2; i++) {
        uint64_t mark = g_nout; int live = tcb_live();
        tcp_frame(PEER_IP, bad_dst[i], 80, F_SYN, 1000, 0);               /* SYN на широковещательный адрес (RFC 1122 п. 4.2.3.10) */
        tcp_frame(PEER_IP, bad_dst[i], 81, F_SYN, 1000, 0);
        EXPECT(g_nout == mark && tcb_live() == live, "TCP на широковещательный адрес %08x: отправлено %llu, соединений %d->%d", bad_dst[i], (unsigned long long)(g_nout - mark), live, tcb_live());
    }
    {   /* контроль: обычному соседу отвечаем */
        uint64_t mark = g_nout; int live = tcb_live();
        tcp_frame(PEER_IP, MY_IP, 80, F_SYN, 1000, 0);
        EXPECT(g_nout == mark + 1 && tcb_live() == live + 1, "SYN от соседа: ответа или соединения нет");
        mark = g_nout; tcp_frame(PEER_IP, MY_IP, 81, F_SYN, 1000, 0);
        EXPECT(g_nout == mark + 1, "SYN на закрытый порт: RST не отправлен");
        mark = g_nout; echo_frame(PEER_IP, MY_IP);
        EXPECT(g_nout == mark + 1, "эхо от соседа: ответа нет");
    }
    /* 2. DHCP: негодный адрес/маска не делают стек узлом 0.0.0.0, 224.x, 255.255.255.255 или адресом сети/подсети (раньше принимались) */
    setup_net_as(0, 0);
    dstate = D_REQUESTING; xid = 0x1234; my_ip = 0;
    static const struct { uint32_t yi, mask; } bad_ack[] = {
        { 0, 0xFFFFFF00u }, { 0xFFFFFFFFu, 0 }, { 0xE0D0B307u, 0 }, { 0x7F000001u, 0 }, { 0xF0000001u, 0 },
        { 0x0A0002FFu, 0xFFFFFF00u }, { 0x0A000200u, 0xFFFFFF00u }, { MY_IP, 0x00FF00FFu }, { MY_IP, 0xFFFFFF01u } };
    for (unsigned i = 0; i < sizeof bad_ack / sizeof bad_ack[0]; i++) {
        dhcp_frame(bad_ack[i].yi, 5, bad_ack[i].mask, PEER_IP);
        EXPECT(dstate == D_REQUESTING && my_ip == 0, "DHCP ACK с адресом %08x маской %08x принят (адрес %08x)", bad_ack[i].yi, bad_ack[i].mask, my_ip);
    }
    dstate = D_SELECTING;
    for (unsigned i = 0; i < sizeof bad_ack / sizeof bad_ack[0]; i++) {
        uint64_t mark = g_nout;
        dhcp_frame(bad_ack[i].yi, 2, bad_ack[i].mask, PEER_IP);
        EXPECT(dstate == D_SELECTING && g_nout == mark, "DHCP OFFER с адресом %08x маской %08x принят", bad_ack[i].yi, bad_ack[i].mask);
    }
    dstate = D_REQUESTING;
    dhcp_frame(MY_IP, 5, 0xFFFFFF00u, 0xE0000005u);                       /* годный адрес, групповой «шлюз» - шлюза нет */
    EXPECT(dstate == D_BOUND && my_ip == MY_IP && my_gw == 0, "DHCP ACK с групповым шлюзом: состояние %d, шлюз %08x", dstate, my_gw);
    /* 3. Смена адреса по DHCP: соединения со старым адресом сбрасываются (раньше повторы шли с адреса, которого у нас уже нет) */
    setup_net_as(1, 0);
    {
        tcp_frame(PEER_IP, MY_IP, 80, F_SYN, 1000, 0);
        const uint8_t* sa = g_out[(g_nout - 1) % OUT_N];
        EXPECT(g_outn[(g_nout - 1) % OUT_N] >= 54 && (sa[47] & (F_SYN | F_ACK)) == (F_SYN | F_ACK), "нет SYN-ACK");
        uint32_t iss = get32(sa + 38);
        tcp_frame(PEER_IP, MY_IP, 80, F_ACK, 1001, iss + 1);
        EXPECT(tcb_live() == 1 && tcbs[0].state == T_EST, "соединение не установилось (состояние %d)", tcbs[0].state);
        dstate = D_REQUESTING; xid = 0x777;
        dhcp_frame(0x0A00024Du, 5, 0xFFFFFF00u, PEER_IP);
        EXPECT(my_ip == 0x0A00024Du && tcbs[0].state == T_CLOSED && tcbs[0].reset, "смена адреса: соединение осталось (состояние %d)", tcbs[0].state);
        g_ticks += 40 * TIMER_HZ; host_tick();                            /* повторы со старым адресом отправителя ловит проверка отправленных кадров */
    }
}

static void run_scenario(uint64_t idx) {
    g_scen = idx;
    rs = (g_seed + idx * 0x9E3779B97F4A7C15ull) | 1; for (int i = 0; i < 4; i++) rnd();
    long live0 = g_live;
    alarm(60);
    setup_net();
    uint32_t r = rr(100);
    if (r < 50) scen_frames(1 + (int)rr(8));
    else if (r < 58) scen_dhcp();
    else if (r < 66) scen_dns();
    else if (r < 78) scen_tcpflow();
    else if (r < 86) scen_frag_clean();
    else if (r < 90) scen_udp_flood();
    else { g_nin = 0; for (int i = 0; i < 1 + (int)rr(12); i++) { gen_kind(30); inject(g_in[0], g_inlen[0]); } }
    if (g_live != live0) BAD("утечка кучи ядра: живых блоков %ld (было %ld)", g_live, live0);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "net_fuzz СЦЕНАРИЕВ ЗЁРНО [С_НОМЕРА]\n"); return 2; }
    long iters = atol(argv[1]);
    g_seed = 0x2545f4914f6cdd1dull ^ strtoull(argv[2], NULL, 0);
    uint64_t first = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;
    signal(SIGALRM, on_fatal); signal(SIGABRT, on_fatal); signal(SIGSEGV, on_fatal);
    regress();
    for (long i = 0; i < iters && !fails; i++) run_scenario(first + (uint64_t)i);
    alarm(0);
    if (fails) { printf("net_fuzz: ПРОВАЛ (%d)\n", fails); return 1; }
    printf("net_fuzz: %ld сценариев, %llu входных кадров, %llu кадров отправлено стеком и проверено - без нарушений\n", iters, (unsigned long long)g_inputs, (unsigned long long)g_sent);
    return 0;
}
