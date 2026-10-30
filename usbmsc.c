#include "usb.h"
#include "blk.h"
#include "part.h"
#include "fat.h"
#include "vfs.h"
#include "devhealth.h"

/* Накопители USB (флешки, кардридеры, внешние диски): Bulk-Only Transport + SCSI (INQUIRY, TEST UNIT READY,
   REQUEST SENSE, READ CAPACITY 10/16, READ/WRITE 10/16, SYNCHRONIZE CACHE). Восстановление по спецификации BOT:
   STALL на данных - CLEAR_FEATURE и чтение CSW; ошибка фазы или испорченный CSW - Reset Recovery.
   Устройство становится блочным (blk_dev_t), первый раздел FAT монтируется в /usbN. Чтение и запись из других
   потоков выполняются потоком usb (usb_call). Отключение: операции сразу получают ошибку, том снимается, как только
   его отпустит последний пользователь. */

#define CHUNK_SECTORS 32                     /* 16 КБ за команду */
#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u

typedef struct {
    int used, gone, vol;
    usb_dev_t* dev;
    usb_pipe_t* in; usb_pipe_t* out;
    uint8_t ep_in, ep_out, iface, lun;
    uint32_t tag;
    uint64_t blocks; uint32_t bsize;
    int cap16;
    blk_dev_t blk;
    fat_vol_t fat;
    char vendor[9], product[17];
    uint32_t errors, got;              /* got - сколько байт данных пришло/ушло в последней команде */
} msc_t;
static msc_t mscs[USB_MAX_MSC];

int msc_count(void) { int n = 0; for (int i = 0; i < USB_MAX_MSC; i++) n += mscs[i].used && !mscs[i].gone; return n; }

static void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void mset(void* d, int c, unsigned n) { uint8_t* p = d; while (n--) *p++ = (uint8_t)c; }

static void reset_recovery(msc_t* m) {
    usb_ctl(m->dev, 0x21, 0xFF, 0, m->iface, 0, 0);                    /* Bulk-Only Mass Storage Reset */
    usb_clear_halt(m->dev, m->in, m->ep_in);
    usb_clear_halt(m->dev, m->out, m->ep_out);
}

/* Одна команда SCSI. 0 - успех, 1 - устройство ответило «ошибка» (смотреть REQUEST SENSE), <0 - сбой транспорта. */
static int bot(msc_t* m, const uint8_t* cb, int cblen, void* data, uint32_t len, int in) {
    uint8_t cbw[31]; mset(cbw, 0, sizeof cbw);
    uint32_t tag = ++m->tag;
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;
    cbw[4] = (uint8_t)tag; cbw[5] = (uint8_t)(tag >> 8); cbw[6] = (uint8_t)(tag >> 16); cbw[7] = (uint8_t)(tag >> 24);
    cbw[8] = (uint8_t)len; cbw[9] = (uint8_t)(len >> 8); cbw[10] = (uint8_t)(len >> 16); cbw[11] = (uint8_t)(len >> 24);
    cbw[12] = in ? 0x80 : 0x00; cbw[13] = m->lun; cbw[14] = (uint8_t)cblen;
    for (int i = 0; i < cblen; i++) cbw[15 + i] = cb[i];
    int r = m->dev->hc->bulk_xfer(m->dev->hc, m->out, cbw, 31);
    if (r != 31) { if (r == USB_ERR_STALL) reset_recovery(m); m->errors++; return -1; }
    m->got = 0;
    if (len) {
        r = m->dev->hc->bulk_xfer(m->dev->hc, in ? m->in : m->out, data, len);
        if (r == USB_ERR_STALL) usb_clear_halt(m->dev, in ? m->in : m->out, in ? m->ep_in : m->ep_out);   /* дальше - CSW */
        else if (r < 0) { reset_recovery(m); m->errors++; return -1; }
        else m->got = (uint32_t)r;
    }
    uint8_t csw[13];
    r = m->dev->hc->bulk_xfer(m->dev->hc, m->in, csw, 13);
    if (r == USB_ERR_STALL) { usb_clear_halt(m->dev, m->in, m->ep_in); r = m->dev->hc->bulk_xfer(m->dev->hc, m->in, csw, 13); }
    if (r != 13) { reset_recovery(m); m->errors++; return -1; }
    uint32_t sig = (uint32_t)csw[0] | (uint32_t)csw[1] << 8 | (uint32_t)csw[2] << 16 | (uint32_t)csw[3] << 24;
    uint32_t ctag = (uint32_t)csw[4] | (uint32_t)csw[5] << 8 | (uint32_t)csw[6] << 16 | (uint32_t)csw[7] << 24;
    if (sig != CSW_SIG || ctag != tag || csw[12] > 2) { reset_recovery(m); m->errors++; return -1; }
    if (csw[12] == 2) { reset_recovery(m); m->errors++; return -1; }  /* ошибка фазы */
    return csw[12];
}

static int sense(msc_t* m, uint8_t* key, uint8_t* asc) {
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 }, s[18]; mset(s, 0, sizeof s);
    int r = bot(m, cb, 6, s, 18, 1);
    *key = s[2] & 15; *asc = s[12];
    return r;
}

static int unit_ready(msc_t* m) {
    for (int i = 0; i < 20; i++) {                                     /* некоторым флешкам нужно до секунды-двух */
        uint8_t cb[6] = { 0 };
        int r = bot(m, cb, 6, 0, 0, 0);
        if (r == 0) return 0;
        uint8_t key, asc;
        if (r < 0 || sense(m, &key, &asc) < 0) return -1;
        if (key == 2 && asc == 0x3A) return -2;                        /* нет носителя (пустой кардридер) */
        usb_sleep_ms(100);
    }
    return -1;
}

static int capacity(msc_t* m) {
    uint8_t cb[10] = { 0x25 }, d[8];
    if (bot(m, cb, 10, d, 8, 1) != 0) return -1;
    uint32_t last = be32(d), bs = be32(d + 4);
    if (last == 0xFFFFFFFFu) {                                         /* больше 2 ТБ: READ CAPACITY(16) */
        uint8_t c16[16] = { 0x9E, 0x10 }, e[32];
        c16[13] = 32;
        if (bot(m, c16, 16, e, 32, 1) != 0) return -1;
        m->blocks = ((uint64_t)be32(e) << 32 | be32(e + 4)) + 1; bs = be32(e + 8); m->cap16 = 1;
    } else m->blocks = (uint64_t)last + 1;
    m->bsize = bs;
    return 0;
}

/* ---- блочное устройство ---- */

typedef struct { msc_t* m; uint64_t lba; uint32_t count; void* buf; int write; } io_t;

static int rw_chunk(msc_t* m, uint64_t lba, uint32_t n, void* buf, int write) {
    uint8_t cb[16]; mset(cb, 0, sizeof cb);
    int len;
    if (lba + n > 0xFFFFFFFFull || m->cap16) {
        cb[0] = write ? 0x8A : 0x88;
        put_be32(cb + 2, (uint32_t)(lba >> 32)); put_be32(cb + 6, (uint32_t)lba); put_be32(cb + 10, n);
        len = 16;
    } else {
        cb[0] = write ? 0x2A : 0x28;
        put_be32(cb + 2, (uint32_t)lba); cb[7] = (uint8_t)(n >> 8); cb[8] = (uint8_t)n;
        len = 10;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        if (m->gone) return -1;
        int r = bot(m, cb, len, buf, n * 512, !write);
        if (r == 0 && m->got == n * 512) return 0;
        if (r == 0) { m->errors++; continue; }                         /* «успех», но данных меньше: повторить */
        if (r > 0) { uint8_t key, asc; sense(m, &key, &asc); if (key == 7) return -1; }   /* защита от записи */
        usb_sleep_ms(20);
    }
    return -1;
}

static int io_fn(void* a) {
    io_t* q = (io_t*)a;
    msc_t* m = q->m;
    for (uint32_t done = 0; done < q->count; ) {
        if (m->gone) return -1;
        uint32_t n = q->count - done > CHUNK_SECTORS ? CHUNK_SECTORS : q->count - done;
        if (rw_chunk(m, q->lba + done, n, (uint8_t*)q->buf + (uint64_t)done * 512, q->write) != 0) return -1;
        done += n;
    }
    return 0;
}

static int blk_rd(blk_dev_t* b, uint64_t lba, uint32_t n, void* buf) {
    msc_t* m = (msc_t*)b->priv;
    if (m->gone || lba + n > m->blocks) return -1;
    io_t q = { m, lba, n, buf, 0 };
    int r = usb_call(io_fn, &q);
    if (r && !m->gone) devhealth_error(m->blk.name + 4);              /* «USB имя» -> «имя» */
    return r;
}
static int blk_wr(blk_dev_t* b, uint64_t lba, uint32_t n, const void* buf) {
    msc_t* m = (msc_t*)b->priv;
    if (m->gone || lba + n > m->blocks) return -1;
    io_t q = { m, lba, n, (void*)buf, 1 };
    int r = usb_call(io_fn, &q);
    if (r && !m->gone) devhealth_error(m->blk.name + 4);
    return r;
}
static int sync_fn(void* a) {
    msc_t* m = (msc_t*)a;
    if (m->gone) return -1;
    uint8_t cb[10] = { 0x35 };
    bot(m, cb, 10, 0, 0, 0);                                           /* не все флешки умеют - не ошибка */
    return 0;
}
static int blk_fl(blk_dev_t* b) { msc_t* m = (msc_t*)b->priv; return m->gone ? -1 : usb_call(sync_fn, m); }

static void copy_str(char* d, const uint8_t* s, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) d[k++] = (char)(s[i] >= 0x20 && s[i] < 0x7F ? s[i] : ' ');
    while (k && d[k - 1] == ' ') k--;
    d[k] = 0;
}

static void mount(msc_t* m) {
    static uint8_t s0[512];
    if (m->blk.read(&m->blk, 0, 1, s0) != 0) { usb_log("[KIRA USB] storage: cannot read sector 0\n"); return; }
    static part_t pt[PART_MAX];                                        /* не на стеке потока usb */
    int np = part_scan(&m->blk, s0, pt, PART_MAX);
    for (int i = 0; i < np; i++) {
        if (fat_mount(&m->fat, &m->blk, pt[i].start, pt[i].count) != 0) continue;
        char desc[48]; int k = 0;
        for (const char* p = m->vendor; *p && k < 20; p++) desc[k++] = *p;
        if (k) desc[k++] = ' ';
        for (const char* p = m->product; *p && k < 46; p++) desc[k++] = *p;
        desc[k] = 0;
        m->vol = vfs_mount_fat(&m->fat, desc);
        if (m->vol < 0) { usb_log("[KIRA USB] storage: no free mount point (4 volumes max)\n"); return; }
        usb_log("[KIRA USB] storage: FAT"); usb_log_dec((uint64_t)m->fat.type);
        if (m->fat.label[0]) { usb_log(" '"); usb_log(m->fat.label); usb_log("'"); }
        usb_log(" mounted at /usb"); usb_log_dec((uint64_t)m->vol); usb_log("\n");
        return;
    }
    if (np == 0 && s0[3] == 'E' && s0[4] == 'X' && s0[5] == 'F' && s0[6] == 'A' && s0[7] == 'T') usb_log("[KIRA USB] storage: exFAT is not supported yet (format the stick as FAT32)\n");
    else if (np == 0 && s0[3] == 'N' && s0[4] == 'T' && s0[5] == 'F' && s0[6] == 'S') usb_log("[KIRA USB] storage: NTFS is not supported yet (format the stick as FAT32)\n");
    else usb_log("[KIRA USB] storage: no FAT volume found (exFAT/NTFS or unformatted)\n");
}

int msc_attach(usb_dev_t* d, const usb_iface_t* f) {
    if (f->sub != 6 || f->proto != 0x50) { usb_log("[KIRA USB] storage with non-SCSI or non-Bulk-Only protocol: not supported\n"); return -1; }
    msc_t* m = 0;
    for (int i = 0; i < USB_MAX_MSC; i++) if (!mscs[i].used) { m = &mscs[i]; break; }
    if (!m) { usb_log("[KIRA USB] too many storage devices\n"); return -1; }
    mset(m, 0, sizeof *m);
    m->dev = d; m->iface = f->num; m->vol = -1;
    for (int k = 0; k < f->nep; k++) {
        if ((f->ep[k].attr & 3) != 2) continue;
        if (f->ep[k].addr & 0x80) { if (!m->ep_in) { m->ep_in = f->ep[k].addr; m->in = d->hc->bulk_open(d->hc, d, f->ep[k].addr, f->ep[k].maxp); } }
        else if (!m->ep_out) { m->ep_out = f->ep[k].addr; m->out = d->hc->bulk_open(d->hc, d, f->ep[k].addr, f->ep[k].maxp); }
    }
    if (!m->in || !m->out) {
        usb_log("[KIRA USB] storage: bulk endpoints not available\n");
        if (m->in && d->hc->pipe_close) d->hc->pipe_close(d->hc, m->in);
        if (m->out && d->hc->pipe_close) d->hc->pipe_close(d->hc, m->out);
        return -1;
    }
    m->used = 1;
    uint8_t maxlun = 0;
    usb_ctl(d, 0xA1, 0xFE, 0, m->iface, 1, &maxlun);                   /* устройство с одной LUN может ответить STALL */
    uint8_t inq[36]; uint8_t cb[6] = { 0x12, 0, 0, 0, 36, 0 };
    mset(inq, 0, sizeof inq);
    if (bot(m, cb, 6, inq, 36, 1) < 0) { usb_log("[KIRA USB] storage: INQUIRY failed\n"); msc_detach(d); return -1; }
    copy_str(m->vendor, inq + 8, 8); copy_str(m->product, inq + 16, 16);
    usb_log("[KIRA USB] storage: "); usb_log(m->vendor); usb_log(" "); usb_log(m->product);
    if ((inq[0] & 0x1F) != 0) { usb_log(" - not a disk (CD/other), skipped\n"); msc_detach(d); return -1; }
    int ur = unit_ready(m);
    if (ur == -2) { usb_log(" - no medium inserted\n"); return 0; }
    if (ur != 0 || capacity(m) != 0) { usb_log(" - not ready\n"); return 0; }
    usb_log(", "); usb_log_dec(m->blocks * m->bsize / (1024 * 1024)); usb_log(" MB\n");
    if (m->bsize != 512) { usb_log("[KIRA USB] storage: sector size is not 512 bytes - not supported yet\n"); return 0; }
    int k = 0; const char* pre = "USB "; while (pre[k]) { m->blk.name[k] = pre[k]; k++; }
    for (const char* p = m->product; *p && k < 46; p++) m->blk.name[k++] = *p;
    m->blk.name[k] = 0;
    m->blk.sectors = m->blocks; m->blk.read = blk_rd; m->blk.write = blk_wr; m->blk.flush = blk_fl; m->blk.priv = m;
    mount(m);
    return 0;
}

void msc_detach(usb_dev_t* d) {
    for (int i = 0; i < USB_MAX_MSC; i++) {
        msc_t* m = &mscs[i];
        if (!m->used || m->dev != d || m->gone) continue;
        m->gone = 1;                                                   /* новые операции - сразу ошибка */
        if (d->hc->pipe_close) { d->hc->pipe_close(d->hc, m->in); d->hc->pipe_close(d->hc, m->out); }
        if (m->vol >= 0) { usb_log("[KIRA USB] storage removed: /usb"); usb_log_dec((uint64_t)m->vol); usb_log(" unmounted\n"); }
        msc_poll();
    }
}

/* Снять том отключённого устройства, когда его никто не держит (иначе - на следующем шаге). */
void msc_poll(void) {
    for (int i = 0; i < USB_MAX_MSC; i++) {
        msc_t* m = &mscs[i];
        if (!m->used || !m->gone) continue;
        if (m->vol >= 0 && vfs_unmount_try(m->vol) != 0) continue;
        m->vol = -1; m->used = 0;
    }
}

void msc_print(void (*out)(const char*)) {
    for (int i = 0; i < USB_MAX_MSC; i++) {
        msc_t* m = &mscs[i];
        if (!m->used || m->gone) continue;
        out("  накопитель: "); out(m->vendor); out(" "); out(m->product);
        if (m->vol >= 0) { char p[8] = { ' ', '/', 'u', 's', 'b', (char)('0' + m->vol), 0 }; out(" ->"); out(p); }
        else out(" (тома FAT нет)");
        out("\n");
    }
}
