/* Модели USB-устройств для тестов стека и контроллеров. Устройство отвечает на транзакции, как настоящее:
   - управляющая точка: SETUP/DATA/STATUS, SET_ADDRESS после стадии статуса, дескрипторы, запросы классов,
     CLEAR_FEATURE(ENDPOINT_HALT);
   - HID: прерывающая IN с очередью отчётов;
   - хаб USB 2.0: дескриптор хаба, питание/сброс/состояние портов, биты изменений, точка «изменения» (битовая карта);
     устройства за его портами доступны, только пока хаб настроен и порт включён;
   - накопитель Bulk-Only + SCSI на RAM-диске: CBW/данные/CSW, INQUIRY, TEST UNIT READY (с «не готов» в начале),
     REQUEST SENSE, READ CAPACITY, READ/WRITE(10), SYNCHRONIZE CACHE; STALL данных, сброс Bulk-Only, защита от записи.
   Везде проверяется чередование DATA0/DATA1 (toggle = -1: контроллер ведёт его сам, проверять нечего). */
#include <string.h>
#include <stdint.h>

enum { MK_HID = 0, MK_HUB = 1, MK_MSC = 2 };
#define MHUB_PORTS 4

typedef struct mdev mdev_t;
struct mdev {
    int kind, connected, low_speed, high_speed, addr, pending_addr, configured, protocol, idle;
    mdev_t* parent; int pport;                         /* хаб и его порт (1..); 0 - корневой порт контроллера */
    const uint8_t *dev, *cfg, *rd; unsigned cfg_len, rd_len;
    uint8_t devbuf[18], cfgbuf[64];
    uint8_t s[8]; uint8_t data[1100]; unsigned data_len, data_pos; int in_dir, stage;  /* 0 нет, 1 данные, 2 статус, -1 STALL */
    uint8_t rep[256][8]; unsigned rlen[256]; int nrep, rpos; int next_toggle;
    int ep0_maxp, setups, bad_toggle;
    /* хаб */
    int nports; uint32_t pst[MHUB_PORTS + 1]; mdev_t* child[MHUB_PORTS + 1]; int resets_done;
    /* накопитель */
    uint8_t* disk; uint32_t nsect; int ro, bulk_maxp;
    int bstate;                                        /* 0 ждём CBW, 1 данные к хосту, 2 данные от хоста, 3 CSW */
    uint32_t tag, dlen, dpos, avail; uint8_t status; const uint8_t* src; uint8_t* dst; uint8_t resp[64];
    int tog_in, tog_out, halt_in, halt_out;
    int notready, stall_data_once, maxlun_stall, nomedium, short_read_once;
    uint8_t key, asc;
    int cbws, bot_resets, clear_halts, reads, writes, syncs, sectors_rd, sectors_wr;
};

#define M_NAK  (-1)
#define M_STALL (-2)

static void mdev_bot_reset(mdev_t* d) { d->bstate = 0; d->dlen = d->dpos = 0; }

static void put32le(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put32be(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void mdev_setup(mdev_t* d, const uint8_t* s) {
    memcpy(d->s, s, 8); d->setups++;
    unsigned rt = s[0], req = s[1], val = s[2] | s[3] << 8, idx = s[4] | s[5] << 8, len = s[6] | s[7] << 8;
    d->in_dir = (rt & 0x80) != 0; d->data_pos = 0; d->data_len = 0;
    const uint8_t* src = 0; unsigned n = 0; uint8_t tmp[16];
    if (rt == 0x80 && req == 6) {
        if ((val >> 8) == 1) { src = d->dev; n = 18; }
        else if ((val >> 8) == 2) { src = d->cfg; n = d->cfg_len; }
    } else if (rt == 0x81 && req == 6 && (val >> 8) == 0x22) { src = d->rd; n = d->rd_len; }
    else if (d->kind == MK_HUB && rt == 0xA0 && req == 6 && (val >> 8) == 0x29) {
        uint8_t hd[9] = { 9, 0x29, (uint8_t)d->nports, 0x00, 0x00, 50, 100, 0, 0xFF };
        memcpy(tmp, hd, 9); src = tmp; n = 9;
    } else if (d->kind == MK_HUB && rt == 0xA3 && req == 0) {               /* состояние порта */
        if (idx < 1 || (int)idx > d->nports) { d->stage = -1; return; }
        uint32_t st = d->pst[idx];
        mdev_t* c = d->child[idx];
        if (c && (st & 1)) st |= c->low_speed ? 1u << 9 : c->high_speed ? 1u << 10 : 0;
        put32le(tmp, st); src = tmp; n = 4;
    } else if (d->kind == MK_HUB && rt == 0x23 && (req == 1 || req == 3)) { /* CLEAR/SET_FEATURE порта */
        if (idx < 1 || (int)idx > d->nports) { d->stage = -1; return; }
        if (req == 3 && val == 8) d->pst[idx] |= 1u << 8;
        else if (req == 3 && val == 4) {                                     /* сброс: порт включён, устройство - адрес 0 */
            if (d->pst[idx] & 1) { d->pst[idx] |= 2u | (1u << 20); if (d->child[idx]) d->child[idx]->addr = 0; d->resets_done++; }
        } else if (req == 1 && val >= 16 && val <= 20) d->pst[idx] &= ~(1u << val);
        else if (req == 1 && val == 1) d->pst[idx] &= ~2u;
        d->stage = 2; return;
    } else if (d->kind == MK_MSC && rt == 0xA1 && req == 0xFE) {            /* Get Max LUN */
        if (d->maxlun_stall) { d->stage = -1; return; }
        tmp[0] = 0; src = tmp; n = 1;
    } else if (d->kind == MK_MSC && rt == 0x21 && req == 0xFF) { d->bot_resets++; mdev_bot_reset(d); d->stage = 2; return; }
    else if (rt == 0x02 && req == 1 && val == 0) {                           /* CLEAR_FEATURE(ENDPOINT_HALT) */
        d->clear_halts++;
        if (idx == 0x81) { d->halt_in = 0; d->tog_in = 0; d->next_toggle = 0; }
        else if (idx == 0x02) { d->halt_out = 0; d->tog_out = 0; }
        d->stage = 2; return;
    } else if (rt == 0x00 && req == 5) { d->pending_addr = (int)val; d->stage = 2; return; }
    else if (rt == 0x00 && req == 9) { d->configured = (int)val; d->stage = 2; return; }
    else if (rt == 0x21 && req == 0x0B) { d->protocol = (int)val; d->stage = 2; return; }
    else if (rt == 0x21 && req == 0x0A) { d->idle = 1; d->stage = 2; return; }
    else { d->stage = -1; return; }
    if (!src) { d->stage = -1; return; }
    if (n > len) n = len;
    memcpy(d->data, src, n); d->data_len = n; d->stage = n ? 1 : 2;
}

/* ---- накопитель: SCSI ---- */
static void msc_command(mdev_t* d, const uint8_t* cbw) {
    d->cbws++;
    d->tag = (uint32_t)cbw[4] | (uint32_t)cbw[5] << 8 | (uint32_t)cbw[6] << 16 | (uint32_t)cbw[7] << 24;
    d->dlen = (uint32_t)cbw[8] | (uint32_t)cbw[9] << 8 | (uint32_t)cbw[10] << 16 | (uint32_t)cbw[11] << 24;
    int in = (cbw[12] & 0x80) != 0;
    const uint8_t* cb = cbw + 15;
    d->dpos = 0; d->avail = 0; d->src = 0; d->dst = 0; d->status = 0;
    uint8_t key = 0, asc = 0;
    switch (cb[0]) {
    case 0x12: {                                                            /* INQUIRY */
        memset(d->resp, 0, 36); d->resp[1] = 0x80; d->resp[2] = 2; d->resp[3] = 2; d->resp[4] = 31;
        memcpy(d->resp + 8, "KIRA    ", 8); memcpy(d->resp + 16, "Model Stick     ", 16); memcpy(d->resp + 32, "1.00", 4);
        d->src = d->resp; d->avail = 36; break; }
    case 0x00:                                                              /* TEST UNIT READY */
        if (d->nomedium) { key = 2; asc = 0x3A; }
        else if (d->notready) { d->notready--; key = 2; asc = 0x04; }
        break;
    case 0x03:                                                              /* REQUEST SENSE */
        memset(d->resp, 0, 18); d->resp[0] = 0x70; d->resp[2] = d->key; d->resp[7] = 10; d->resp[12] = d->asc;
        d->key = d->asc = 0;
        d->src = d->resp; d->avail = 14; break;                            /* меньше запрошенного: короткий пакет + остаток в CSW */
    case 0x25:                                                              /* READ CAPACITY(10) */
        if (d->nomedium) { key = 2; asc = 0x3A; break; }
        put32be(d->resp, d->nsect - 1); put32be(d->resp + 4, 512);
        d->src = d->resp; d->avail = 8; break;
    case 0x28: case 0x2A: {                                                 /* READ(10) / WRITE(10) */
        uint32_t lba = (uint32_t)cb[2] << 24 | (uint32_t)cb[3] << 16 | (uint32_t)cb[4] << 8 | cb[5];
        uint32_t cnt = (uint32_t)cb[7] << 8 | cb[8];
        if ((uint64_t)lba + cnt > d->nsect || d->dlen != cnt * 512u || in != (cb[0] == 0x28)) { key = 5; asc = 0x21; break; }
        if (cb[0] == 0x2A && d->ro) { key = 7; asc = 0x27; break; }
        if (cb[0] == 0x28) {
            d->src = d->disk + (uint64_t)lba * 512; d->avail = cnt * 512; d->reads++; d->sectors_rd += (int)cnt;
            if (d->short_read_once && cnt > 1) { d->short_read_once = 0; d->avail = cnt * 256 + 100; }   /* отдал меньше, чем обещал */
        }
        else { d->dst = d->disk + (uint64_t)lba * 512; d->writes++; d->sectors_wr += (int)cnt; }
        break; }
    case 0x35: d->syncs++; break;                                           /* SYNCHRONIZE CACHE */
    case 0x1E: break;                                                       /* PREVENT ALLOW MEDIUM REMOVAL */
    default: key = 5; asc = 0x20; break;
    }
    if (key) { d->key = key; d->asc = asc; d->status = 1; d->avail = 0; d->src = 0; d->dst = 0; }
    d->bstate = d->dlen ? (in ? 1 : 2) : 3;
}

/* IN-транзакция: число байт, M_NAK или M_STALL. toggle - DATA0/1 из запроса хоста или -1. */
static int mdev_in(mdev_t* d, int ep, uint8_t* buf, unsigned max, int toggle) {
    if (ep == 0) {
        if (d->stage == -1) return M_STALL;
        if (d->stage == 1 && d->in_dir) {
            unsigned n = d->data_len - d->data_pos; if (n > max) n = max; if (n > (unsigned)d->ep0_maxp) n = (unsigned)d->ep0_maxp;
            memcpy(buf, d->data + d->data_pos, n); d->data_pos += n;
            if (d->data_pos >= d->data_len || n < (unsigned)d->ep0_maxp) d->stage = 2;
            return (int)n;
        }
        if (d->stage == 2 && !d->in_dir) {                    /* стадия статуса запроса без данных / OUT */
            d->stage = 0;
            if (d->pending_addr) { d->addr = d->pending_addr; d->pending_addr = 0; }
            return 0;
        }
        return M_STALL;
    }
    if (d->kind == MK_HUB) {
        uint8_t bits = 0;
        for (int p = 1; p <= d->nports; p++) if (d->pst[p] & 0x1F0000u) bits |= (uint8_t)(1u << p);
        if (!bits) return M_NAK;
        if (toggle >= 0 && toggle != d->next_toggle) d->bad_toggle++;
        d->next_toggle ^= 1;
        buf[0] = bits;
        return 1;
    }
    if (d->kind == MK_MSC) {
        if (ep != 1 || d->halt_in) return M_STALL;
        int n;
        if (d->bstate == 1) {
            if (d->stall_data_once) {                          /* устройство отказывается отдавать данные */
                d->stall_data_once = 0; d->halt_in = 1; d->status = 1; d->key = 3; d->asc = 0x11; d->bstate = 3; return M_STALL;
            }
            unsigned left = d->avail > d->dpos ? d->avail - d->dpos : 0, want = d->dlen - d->dpos;
            unsigned mp = (unsigned)d->bulk_maxp;
            unsigned c = want < mp ? want : mp; if (c > max) c = max; if (c > left) c = left;
            if (c) memcpy(buf, d->src + d->dpos, c);
            d->dpos += c;
            if (d->dpos >= d->dlen || c < mp) d->bstate = 3;   /* всё отдано или короткий пакет - дальше CSW */
            n = (int)c;
        } else if (d->bstate == 3) {
            if (max < 13) return M_STALL;
            buf[0] = 0x55; buf[1] = 0x53; buf[2] = 0x42; buf[3] = 0x53;
            put32le(buf + 4, d->tag); put32le(buf + 8, d->dlen - (d->dpos < d->dlen ? d->dpos : d->dlen)); buf[12] = d->status;
            d->bstate = 0;
            n = 13;
        } else return M_NAK;
        if (toggle >= 0 && toggle != d->tog_in) d->bad_toggle++;
        d->tog_in ^= 1;
        return n;
    }
    if (d->rpos >= d->nrep) return M_NAK;
    if (toggle >= 0 && toggle != d->next_toggle) d->bad_toggle++;
    d->next_toggle ^= 1;
    unsigned n = d->rlen[d->rpos]; if (n > max) n = max;
    memcpy(buf, d->rep[d->rpos++], n);
    return (int)n;
}

static int mdev_out(mdev_t* d, int ep, const uint8_t* buf, unsigned n, int toggle) {
    if (ep == 0) {
        if (d->stage == 2 && d->in_dir && n == 0) { d->stage = 0; return 0; }
        if (d->stage == 1 && !d->in_dir) { d->data_pos += n; if (d->data_pos >= d->data_len) d->stage = 2; return 0; }
        return M_STALL;
    }
    if (d->kind != MK_MSC || ep != 2 || d->halt_out) return M_STALL;
    if (toggle >= 0 && toggle != d->tog_out) d->bad_toggle++;
    d->tog_out ^= 1;
    if (d->bstate == 0) {
        if (n != 31 || buf[0] != 0x55 || buf[1] != 0x53 || buf[2] != 0x42 || buf[3] != 0x43) { d->halt_in = d->halt_out = 1; return 0; }   /* неверный CBW */
        msc_command(d, buf);
        return 0;
    }
    if (d->bstate == 2) {
        unsigned c = n; if (c > d->dlen - d->dpos) c = d->dlen - d->dpos;
        if (d->dst) memcpy(d->dst + d->dpos, buf, c);
        d->dpos += c;
        if (d->dpos >= d->dlen) d->bstate = 3;
        return 0;
    }
    return M_STALL;
}

static void mdev_report(mdev_t* d, const uint8_t* r, unsigned n) { memcpy(d->rep[d->nrep], r, n); d->rlen[d->nrep++] = n; }

/* Доступно ли устройство на шине: подключено, а за хабом - хаб настроен и порт включён. */
static int mdev_behind_ok(const mdev_t* d) {
    if (!d->connected) return 0;
    if (!d->parent) return 1;
    return mdev_behind_ok(d->parent) && d->parent->configured && (d->parent->pst[d->pport] & 2);
}

/* ---- хаб: подключить / отключить устройство на его порту ---- */
static void mhub_plug(mdev_t* h, int port, mdev_t* c) {
    c->parent = h; c->pport = port; c->connected = 1; c->addr = 0;
    h->child[port] = c;
    h->pst[port] = (h->pst[port] & ~2u) | 1u | (1u << 16);
}
static void mhub_unplug(mdev_t* h, int port) {
    if (h->child[port]) h->child[port]->connected = 0;
    h->child[port] = 0;
    h->pst[port] = (h->pst[port] & ~3u) | (1u << 16);
}

static const uint8_t m_tablet_rd[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x09,0x01, 0xa1,0x00, 0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01,
    0x95,0x03, 0x75,0x01, 0x81,0x02, 0x95,0x01, 0x75,0x05, 0x81,0x01, 0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00,
    0x26,0xff,0x7f, 0x35,0x00, 0x46,0xff,0x7f, 0x75,0x10, 0x95,0x02, 0x81,0x02, 0x05,0x01, 0x09,0x38, 0x15,0x81,
    0x25,0x7f, 0x35,0x00, 0x45,0x00, 0x75,0x08, 0x95,0x01, 0x81,0x06, 0xc0, 0xc0 };
static const uint8_t m_kbd_dev[18] = { 18, 1, 0x10, 0x01, 0, 0, 0, 8, 0x27, 0x06, 0x01, 0x00, 0, 1, 1, 2, 3, 1 };
static const uint8_t m_tab_dev[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x27, 0x06, 0x01, 0x00, 0, 1, 1, 2, 3, 1 };
static const uint8_t m_kbd_cfg[] = { 9, 2, 34, 0, 1, 1, 0, 0xA0, 50,  9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 1, 0, 1, 0x22, 63, 0,  7, 5, 0x81, 3, 8, 0, 10 };
static const uint8_t m_tab_cfg[] = { 9, 2, 34, 0, 1, 1, 0, 0xA0, 50,  9, 4, 0, 0, 1, 3, 0, 0, 0,
    9, 0x21, 0x01, 1, 0, 1, 0x22, sizeof m_tablet_rd, 0,  7, 5, 0x81, 3, 6, 0, 10 };

static void mdev_kbd(mdev_t* d) { memset(d, 0, sizeof *d); d->connected = 1; d->low_speed = 1; d->dev = m_kbd_dev; d->cfg = m_kbd_cfg; d->cfg_len = sizeof m_kbd_cfg; d->ep0_maxp = 8; }
static void mdev_tablet(mdev_t* d) { memset(d, 0, sizeof *d); d->connected = 1; d->dev = m_tab_dev; d->cfg = m_tab_cfg; d->cfg_len = sizeof m_tab_cfg; d->rd = m_tablet_rd; d->rd_len = sizeof m_tablet_rd; d->ep0_maxp = 64; }

/* Хаб на 4 порта: hs - USB 2.0 с одним переводчиком (протокол 1), иначе FS-хаб. */
static void mdev_hub(mdev_t* d, int hs) {
    memset(d, 0, sizeof *d);
    d->kind = MK_HUB; d->connected = 1; d->high_speed = hs; d->nports = MHUB_PORTS; d->ep0_maxp = 64;
    uint8_t dd[18] = { 18, 1, 0x00, 0x02, 9, 0, (uint8_t)(hs ? 1 : 0), 64, 0x09, 0x04, 0x5A, 0x25, 0, 1, 0, 0, 0, 1 };
    memcpy(d->devbuf, dd, 18); d->dev = d->devbuf;
    uint8_t cf[] = { 9, 2, 25, 0, 1, 1, 0, 0xE0, 0,  9, 4, 0, 0, 1, 9, 0, 0, 0,  7, 5, 0x81, 3, 1, 0, (uint8_t)(hs ? 12 : 255) };
    memcpy(d->cfgbuf, cf, sizeof cf); d->cfg = d->cfgbuf; d->cfg_len = sizeof cf;
}

/* Флешка на RAM-диске: hs - USB 2.0 (пакеты 512), иначе FS (64). */
static void mdev_stick(mdev_t* d, uint8_t* disk, uint32_t nsect, int hs) {
    memset(d, 0, sizeof *d);
    d->kind = MK_MSC; d->connected = 1; d->high_speed = hs; d->ep0_maxp = 64;
    d->disk = disk; d->nsect = nsect; d->bulk_maxp = hs ? 512 : 64;
    uint8_t dd[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x81, 0x07, 0x67, 0x55, 0, 1, 1, 2, 3, 1 };
    memcpy(d->devbuf, dd, 18); d->dev = d->devbuf;
    uint8_t mlo = (uint8_t)d->bulk_maxp, mhi = (uint8_t)(d->bulk_maxp >> 8);
    uint8_t cf[] = { 9, 2, 32, 0, 1, 1, 0, 0x80, 50,  9, 4, 0, 0, 2, 8, 6, 0x50, 0,
                     7, 5, 0x81, 2, mlo, mhi, 0,  7, 5, 0x02, 2, mlo, mhi, 0 };
    memcpy(d->cfgbuf, cf, sizeof cf); d->cfg = d->cfgbuf; d->cfg_len = sizeof cf;
}

/* Образ флешки: MBR, раздел FAT16 с 2048-го сектора, метка KIRASTICK, файл HELLO.TXT = "privet, kira". */
#define MSTICK_SECTORS 16384u
static uint8_t* mstick_image(void) {
    static uint8_t img[MSTICK_SECTORS * 512];
    memset(img, 0, sizeof img);
    uint32_t start = 2048, count = MSTICK_SECTORS - start;
    uint8_t* mbr = img;
    mbr[446 + 4] = 0x06; put32le(mbr + 446 + 8, start); put32le(mbr + 446 + 12, count);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    uint8_t* bs = img + (uint64_t)start * 512;
    const uint32_t spc = 2, rsvd = 1, nfats = 2, rootents = 512, fatsz = 28;
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90; memcpy(bs + 3, "KIRAOS  ", 8);
    bs[11] = 0x00; bs[12] = 0x02; bs[13] = (uint8_t)spc; bs[14] = (uint8_t)rsvd; bs[16] = (uint8_t)nfats;
    bs[17] = (uint8_t)rootents; bs[18] = (uint8_t)(rootents >> 8);
    bs[19] = (uint8_t)count; bs[20] = (uint8_t)(count >> 8); bs[21] = 0xF8; bs[22] = (uint8_t)fatsz;
    bs[24] = 63; bs[26] = 255; put32le(bs + 28, start);
    bs[36] = 0x80; bs[38] = 0x29; put32le(bs + 39, 0x4B495241u); memcpy(bs + 43, "KIRASTICK  ", 11); memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    for (uint32_t f = 0; f < nfats; f++) {
        uint8_t* fat = bs + (rsvd + f * fatsz) * 512;
        fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF; fat[3] = 0xFF; fat[4] = 0xFF; fat[5] = 0xFF;   /* кластер 2 - конец цепочки */
    }
    uint8_t* root = bs + (rsvd + nfats * fatsz) * 512;
    memcpy(root, "KIRASTICK  ", 11); root[11] = 0x08;
    memcpy(root + 32, "HELLO   TXT", 11); root[32 + 11] = 0x20; root[32 + 26] = 2; put32le(root + 32 + 28, 12);
    uint8_t* data = root + rootents * 32;
    memcpy(data, "privet, kira", 12);
    return img;
}
