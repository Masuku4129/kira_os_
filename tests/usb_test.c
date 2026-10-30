// Ядро USB-стека на модели контроллера уровня передач: устройства (tests/usbdev_model.h) отвечают на транзакции.
// Проверяется: порядок настройки HID (дескрипторы, SET_CONFIGURATION, SET_PROTOCOL, SET_IDLE, дескриптор отчёта),
// отчёты в очередь ввода, автоповтор, отключение с зажатой клавишей; хабы (HS с переводчиком и FS за ним):
// питание, сброс портов, скорость, маршрут и TT для xHCI, подключение/отключение по точке изменений, отключение
// хаба вместе со всем деревом; флешка: Bulk-Only + SCSI (не готова в начале, STALL на данных, защита от записи,
// нет носителя), MBR + FAT16 -> /usb0 в VFS, чтение и запись больших файлов кусками, извлечение.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb.h"
#include "input.h"
#include "vfs.h"
#include "devhealth.h"
#include "usbdev_model.h"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n%s\n",__LINE__,#c,logbuf); exit(1);} }while(0)

static char logbuf[32768];
static uint32_t now;
void* usb_dma_alloc(uint32_t b, uint64_t* p, int lo) { (void)lo; void* m = calloc(1, b); *p = (uint64_t)(uintptr_t)m; return m; }
void usb_sleep_ms(uint32_t ms) { now += ms; }
uint32_t usb_now_ms(void) { return now; }
void usb_log(const char* s) { strncat(logbuf, s, sizeof logbuf - strlen(logbuf) - 1); }
void usb_log_dec(uint64_t v) { char b[24]; snprintf(b, sizeof b, "%llu", (unsigned long long)v); usb_log(b); }
void usb_log_hex(uint64_t v) { char b[24]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)v); usb_log(b); }

/* ---- контроллер уровня передач ---- */
static mdev_t* root[5];                         /* корневые порты 1..4 */
static struct { usb_dev_t* d; mdev_t* m; } map[USB_MAX_DEV * 4]; static int nmap;
static int next_addr = 1, next_slot = 1, closed, released, resets_bulk;
static char trace[4096];

static mdev_t* model_of(usb_dev_t* d) { for (int i = nmap - 1; i >= 0; i--) if (map[i].d == d) return map[i].m; return 0; }

static int fake_control(usb_hc_t* hc, usb_dev_t* d, const usb_setup_t* s, void* data) {
    (void)hc;
    mdev_t* m = model_of(d);
    if (!m || !mdev_behind_ok(m)) return USB_ERR;
    char t[48]; snprintf(t, sizeof t, "%d:%02x/%02x/%x ", d->addr, s->bmRequestType, s->bRequest, s->wValue); strncat(trace, t, sizeof trace - strlen(trace) - 1);
    mdev_setup(m, (const uint8_t*)s);
    int in = (s->bmRequestType & 0x80) != 0; unsigned len = s->wLength, got = 0;
    if (len && in) {
        while (got < len) {
            int r = mdev_in(m, 0, (uint8_t*)data + got, len - got, -1);
            if (r < 0) return USB_ERR_STALL;
            got += (unsigned)r;
            if (r < m->ep0_maxp) break;
        }
    } else if (len) { if (mdev_out(m, 0, data, len, -1) < 0) return USB_ERR_STALL; got = len; }
    int r = (in && len) ? mdev_out(m, 0, 0, 0, -1) : mdev_in(m, 0, 0, 0, -1);
    if (r < 0) return USB_ERR_STALL;
    return (int)got;
}
static usb_dev_t* fake_enumerate(usb_hc_t* hc, usb_dev_t* parent, uint8_t port, uint8_t speed) {
    mdev_t* m = parent ? model_of(parent)->child[port] : root[port];
    if (!m || !mdev_behind_ok(m)) return 0;
    uint8_t want = m->low_speed ? USB_SPEED_LOW : m->high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL;
    if (parent && parent->speed != USB_SPEED_HIGH && want == USB_SPEED_HIGH) want = USB_SPEED_FULL;
    if (speed != want) { printf("speed %d, model %d\n", speed, want); exit(1); }
    usb_dev_t* d = usb_dev_new(hc, parent, port, speed);
    if (!d) return 0;
    d->addr = (uint8_t)next_addr++; d->hc_slot = (uint32_t)next_slot++;
    m->addr = d->addr;
    map[nmap].d = d; map[nmap].m = m; nmap++;
    return d;
}
struct usb_pipe { mdev_t* m; uint8_t ep; int open, bulk; };
static struct usb_pipe pipes[64]; static int npipes;
static usb_pipe_t* fake_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp, uint8_t iv) {
    (void)hc; (void)maxp; (void)iv;
    struct usb_pipe* p = &pipes[npipes++]; p->m = model_of(d); p->ep = ep; p->open = 1; p->bulk = 0; return p;
}
static int fake_poll(usb_hc_t* hc, usb_pipe_t* p, uint8_t* buf, unsigned cap) {
    (void)hc;
    if (!p->open) return -1;
    if (!mdev_behind_ok(p->m)) return -1;                       /* устройство не отвечает */
    int r = mdev_in(p->m, p->ep & 15, buf, cap, -1);
    return r == M_NAK ? 0 : r < 0 ? -1 : r;
}
static usb_pipe_t* fake_bulk_open(usb_hc_t* hc, usb_dev_t* d, uint8_t ep, uint16_t maxp) {
    (void)hc; (void)maxp;
    struct usb_pipe* p = &pipes[npipes++]; p->m = model_of(d); p->ep = ep; p->open = 1; p->bulk = 1; return p;
}
static int fake_bulk(usb_hc_t* hc, usb_pipe_t* p, void* buf, unsigned len) {
    (void)hc;
    if (!p->open || !p->bulk || !mdev_behind_ok(p->m)) return USB_ERR;
    unsigned mp = (unsigned)p->m->bulk_maxp, done = 0;
    if (p->ep & 0x80) {
        while (done < len) {
            unsigned c = len - done < mp ? len - done : mp;
            int r = mdev_in(p->m, p->ep & 15, (uint8_t*)buf + done, c, -1);
            if (r == M_STALL) return USB_ERR_STALL;
            if (r == M_NAK) return USB_ERR;
            done += (unsigned)r;
            if ((unsigned)r < c) break;
        }
    } else {
        do {
            unsigned c = len - done < mp ? len - done : mp;
            if (mdev_out(p->m, p->ep & 15, (const uint8_t*)buf + done, c, -1) < 0) return USB_ERR_STALL;
            done += c;
        } while (done < len);
    }
    return (int)done;
}
static void fake_bulk_reset(usb_hc_t* hc, usb_pipe_t* p) { (void)hc; (void)p; resets_bulk++; }
static void fake_close(usb_hc_t* hc, usb_pipe_t* p) { (void)hc; CHECK(p->open); p->open = 0; closed++; }
static void fake_release(usb_hc_t* hc, usb_dev_t* d) { (void)hc; (void)d; released++; }

static usb_hc_t hc = { .name = "FAKE", .control = fake_control, .enumerate = fake_enumerate, .intr_open = fake_open,
    .intr_poll = fake_poll, .bulk_open = fake_bulk_open, .bulk_xfer = fake_bulk, .bulk_reset = fake_bulk_reset,
    .pipe_close = fake_close, .release = fake_release };

static usb_dev_t* plug_root(int port, mdev_t* m) {
    root[port] = m;
    usb_dev_t* d = fake_enumerate(&hc, 0, (uint8_t)port, m->low_speed ? USB_SPEED_LOW : m->high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL);
    if (d) usb_attach(d);
    return d;
}
static void spin(int n) { for (int i = 0; i < n; i++) { now += 4; usb_hid_poll(now); hub_poll(); msc_poll(); } }
static usb_dev_t* dev_of(mdev_t* m) { for (int i = nmap - 1; i >= 0; i--) if (map[i].m == m && map[i].d->used) return map[i].d; return 0; }
static int typed(const char* want) {
    char t[32] = { 0 }; int n = 0; input_event_t ev;
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch && n < 31) t[n++] = (char)ev.ch;
    return !strcmp(t, want);
}
static int img_read(blk_dev_t* d, uint64_t l, uint32_t c, void* o) { memcpy(o, (uint8_t*)d->priv + l * 512, (size_t)c * 512); return 0; }
static void key(mdev_t* k, uint8_t usage) { uint8_t r[8] = { 0, 0, usage }, z[8] = { 0 }; mdev_report(k, r, 8); mdev_report(k, z, 8); }

int main(void) {
    input_init();
    static mdev_t kbd, tab, hub, fshub, kbd2, kbd3, stick, ro, empty;

    /* ---- HID на корневых портах ---- */
    mdev_kbd(&kbd); mdev_tablet(&tab);
    usb_dev_t* dk = plug_root(1, &kbd);
    CHECK(dk && dk->ep0_maxp == 8 && kbd.configured == 1 && kbd.protocol == 0 && kbd.idle);   /* загрузочный протокол */
    usb_dev_t* dt = plug_root(2, &tab);
    CHECK(dt && tab.configured == 1 && tab.idle && strstr(trace, ":81/06/2200"));            /* дескриптор отчёта прочитан */
    CHECK(usb_hid_count() == 2 && strstr(logbuf, "keyboard ready") && strstr(logbuf, "absolute"));
    key(&kbd, 0x0B); key(&kbd, 0x0C); key(&kbd, 0x28);
    uint8_t t1[6] = { 1, 0xFF, 0x3F, 0xFF, 0x3F, 0 };
    mdev_report(&tab, t1, 6);
    usb_hid_poll(now);
    CHECK(typed("hi\n"));
    mouse_event_t me;
    CHECK(input_pop_mouse(&me) && me.abs && me.buttons == 1 && me.ax == 16383 && me.ay == 16383);
    CHECK(!input_pop_mouse(&me));
    /* автоповтор: держим «a» */
    uint8_t r_a[8] = { 0, 0, 0x04 };
    mdev_report(&kbd, r_a, 8);
    usb_hid_poll(now);
    for (int t = 0; t < 600; t += 2) { now += 2; usb_hid_poll(now); }
    int downs = 0; input_event_t ev;
    while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN && ev.ch == 'a') downs++;
    CHECK(downs >= 3 && downs <= 5);                                     /* 500 мс тишины, потом каждые 33 мс */
    /* клавиатуру выдернули с зажатой «a»: канал закрыт, клавиша отпущена, устройство освобождено */
    kbd.connected = 0;
    usb_hid_poll(now);
    int ups = 0; while (input_pop(&ev)) if (ev.type == INPUT_KEY_UP && ev.key == 'a') ups++;
    CHECK(ups == 1 && closed == 1 && released == 1 && usb_hid_count() == 1 && !usb_dev_on_port(&hc, 1));
    usb_detach_port(&hc, 2);
    CHECK(usb_hid_count() == 0 && closed == 2 && released == 2);

    /* ---- хаб USB 2.0 на корневом порту 3: за ним LS-клавиатура, флешка и FS-хаб с ещё одной клавиатурой ---- */
    uint8_t* img = mstick_image();
    mdev_hub(&hub, 1); mdev_kbd(&kbd2); mdev_hub(&fshub, 0); mdev_kbd(&kbd3); mdev_stick(&stick, img, MSTICK_SECTORS, 1);
    stick.notready = 3;                                                  /* флешка «просыпается» не сразу */
    mhub_plug(&hub, 1, &kbd2); mhub_plug(&hub, 2, &stick); mhub_plug(&hub, 3, &fshub); mhub_plug(&fshub, 1, &kbd3);
    usb_dev_t* dh = plug_root(3, &hub);
    CHECK(dh && dh->is_hub && dh->hub_nports == 4 && strstr(logbuf, "hub with 4 ports ready"));
    for (int p = 1; p <= 4; p++) CHECK(hub.pst[p] & (1u << 8));          /* питание портов */
    spin(10);
    usb_dev_t *dk2 = dev_of(&kbd2), *ds = dev_of(&stick), *dfh = dev_of(&fshub), *dk3 = dev_of(&kbd3);
    CHECK(dk2 && ds && dfh && dk3);
    CHECK(dk2->speed == USB_SPEED_LOW && dk2->parent == dh && dk2->route == 1 && dk2->root_port == 3 && dk2->depth == 1);
    CHECK(dk2->tt_slot == dh->hc_slot && dk2->tt_port == 1);             /* LS за HS-хабом - через его TT */
    CHECK(ds->speed == USB_SPEED_HIGH && ds->route == 2 && ds->tt_slot == 0);
    CHECK(dfh->speed == USB_SPEED_FULL && dfh->is_hub && dfh->tt_slot == dh->hc_slot && dfh->tt_port == 3);
    CHECK(dk3->route == (3u | 1u << 4) && dk3->depth == 2 && dk3->tt_slot == dh->hc_slot && dk3->tt_port == 3);   /* TT унаследован */
    CHECK(strstr(logbuf, "port 3.3.1") && hub.resets_done == 3 && fshub.resets_done == 1);
    CHECK(usb_hid_count() == 2 && hub_count() == 2 && msc_count() == 1);
    key(&kbd2, 0x1B); key(&kbd3, 0x1C);                                  /* «x» и «y» с клавиатур за хабами */
    spin(2);
    CHECK(typed("xy"));

    /* ---- флешка ---- */
    CHECK(strstr(logbuf, "KIRA Model Stick, 8 MB") && strstr(logbuf, "FAT16 'KIRASTICK' mounted at /usb0"));
    CHECK(stick.notready == 0 && stick.bad_toggle == 0);
    char buf[64] = { 0 };
    CHECK(vfs_read("/usb0/hello.txt", 0, buf, sizeof buf) == 12 && !strcmp(buf, "privet, kira"));
    vfs_info_t vi[4];
    CHECK(vfs_mounts(vi, 4) == 1 && !strcmp(vi[0].label, "KIRASTICK") && strstr(vi[0].desc, "Model Stick"));
    /* большой файл: несколько команд READ/WRITE по 32 сектора */
    static uint8_t big[100000], back[100000];
    for (unsigned i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7 + (i >> 9));
    int w0 = stick.writes;
    CHECK(vfs_write("/usb0/Документы.bin", big, sizeof big) == 0);
    CHECK(stick.writes - w0 >= 7 && stick.syncs >= 1);
    CHECK(vfs_read("/usb0/Документы.bin", 0, back, sizeof back) == (int64_t)sizeof back && !memcmp(big, back, sizeof big));
    /* устройство отказалось отдать данные (STALL): CLEAR_FEATURE, CSW «ошибка», REQUEST SENSE, повтор */
    int ch0 = stick.clear_halts;
    stick.stall_data_once = 1;
    memset(back, 0, sizeof back);
    CHECK(vfs_mkdir("/usb0/папка") == 0);                                 /* каталог читается и пишется после сбоя */
    CHECK(stick.stall_data_once == 0 && stick.clear_halts > ch0 && resets_bulk > 0);
    CHECK(vfs_read("/usb0/Документы.bin", 50000, back, 1000) == 1000 && !memcmp(back, big + 50000, 1000));
    /* испорченный обмен (ошибка фазы) - Reset Recovery: сброс Bulk-Only и обеих точек */
    int br0 = stick.bot_resets;
    stick.halt_out = 1;                                                  /* CBW получит STALL */
    CHECK(vfs_read("/usb0/hello.txt", 0, buf, 12) == 12);
    CHECK(stick.bot_resets > br0 && stick.halt_out == 0);
    /* образ - корректная FAT: независимое монтирование видит записанное */
    {
        blk_dev_t b; memset(&b, 0, sizeof b);
        b.sectors = MSTICK_SECTORS; b.priv = img; b.read = img_read;
        static fat_vol_t v2;
        CHECK(fat_mount(&v2, &b, 2048, MSTICK_SECTORS - 2048) == 0);
        kfs_stat_t st;
        CHECK(fat_stat(&v2, "/Документы.bin", &st) == 0 && st.size == sizeof big);
        CHECK(fat_stat(&v2, "/папка", &st) == 0 && st.type == KFS_DIR);
    }

    /* ---- извлечение: флешку вынули из хаба ---- */
    mhub_unplug(&hub, 2);
    spin(3);
    CHECK(msc_count() == 0 && strstr(logbuf, "storage removed: /usb0 unmounted"));
    CHECK(vfs_read("/usb0/hello.txt", 0, buf, 12) < 0 && vfs_mounts(vi, 4) == 0);
    /* вставили обратно - снова /usb0 */
    mdev_stick(&stick, img, MSTICK_SECTORS, 1);
    mhub_plug(&hub, 2, &stick);
    spin(3);
    CHECK(msc_count() == 1 && vfs_read("/usb0/hello.txt", 0, buf, 12) == 12);

    /* ---- защищённая от записи и пустой кардридер на FS-хабе ---- */
    static uint8_t img2[MSTICK_SECTORS * 512];
    memcpy(img2, img, sizeof img2);
    mdev_stick(&ro, img2, MSTICK_SECTORS, 0); ro.ro = 1;
    mdev_stick(&empty, 0, 0, 0); empty.nomedium = 1; empty.maxlun_stall = 1;
    mhub_plug(&fshub, 2, &ro); mhub_plug(&fshub, 3, &empty);
    spin(4);
    CHECK(msc_count() == 3 && strstr(logbuf, "no medium inserted"));
    CHECK(strstr(logbuf, "mounted at /usb1") && vfs_read("/usb1/hello.txt", 0, buf, 12) == 12);
    CHECK(vfs_write("/usb1/x.txt", "x", 1) < 0 && ro.writes == 0);
    CHECK(ro.bad_toggle == 0 && empty.bad_toggle == 0);

    /* ---- весь хаб выдернули: всё дерево отключено ---- */
    int rel0 = released;
    hub.connected = 0;
    spin(2);
    CHECK(usb_hid_count() == 0 && hub_count() == 0 && msc_count() == 0 && released - rel0 == 7);
    CHECK(vfs_mounts(vi, 4) == 0 && strstr(logbuf, "hub stopped responding"));
    for (int i = 0; i < npipes; i++) CHECK(!pipes[i].open);              /* ни одного забытого канала */

    /* ---- HID без дескриптора отчёта, но загрузочная мышь; сломанный дескриптор конфигурации ---- */
    mdev_tablet(&tab);
    static uint8_t mc[] = { 9, 2, 34, 0, 1, 1, 0, 0xA0, 50,  9, 4, 0, 0, 1, 3, 1, 2, 0,  9, 0x21, 0x01, 1, 0, 1, 0x22, 50, 0,  7, 5, 0x81, 3, 4, 0, 10 };
    tab.cfg = mc; tab.cfg_len = sizeof mc; tab.rd = 0; tab.low_speed = 1;
    CHECK(plug_root(2, &tab) && tab.protocol == 0);
    uint8_t mr[4] = { 2, 0xFE, 3, 0 };
    mdev_report(&tab, mr, 3);
    usb_hid_poll(now);
    CHECK(input_pop_mouse(&me) && !me.abs && me.dx == -2 && me.dy == 3 && me.buttons == 2);
    usb_detach_port(&hc, 2);
    static uint8_t badc[sizeof mc]; memcpy(badc, mc, sizeof mc); badc[1] = 7; tab.cfg = badc;
    usb_dev_t* dm = plug_root(2, &tab);
    CHECK(dm && usb_hid_count() == 0);
    /* ---- иммунитет ввода: «флешка», которая печатает сама (BadUSB) ---- */
    {
        static mdev_t bad, bad2;
        usb_detach_port(&hc, 2);
        while (input_pop(&ev)) {}
        mdev_kbd(&bad); bad.low_speed = 0;
        CHECK(plug_root(4, &bad));
        for (int i = 0; i < 20; i++) key(&bad, (uint8_t)(0x04 + i % 20));     /* 20 разных нажатий подряд */
        for (int i = 0; i < 8; i++) usb_hid_poll(now);                          /* всё в одном окне 200 мс */
        bad.nrep = bad.rpos = 0;
        uint32_t n, rate; char nm[16];
        devhealth_hid_take(&n, &rate, nm);
        CHECK(n == 0 && typed("abcdefghijklmnopqrst"));                         /* 20 за 200 мс: ещё человек (100/с) */
        for (int i = 0; i < 30; i++) key(&bad, (uint8_t)(0x04 + i % 26));
        for (int i = 0; i < 10; i++) usb_hid_poll(now);
        bad.nrep = bad.rpos = 0;
        devhealth_hid_take(&n, &rate, nm);
        int got = 0; while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN) got++;
        CHECK(n == 1 && rate > 120 && !strcmp(nm, "kbd 0627:0001") && got <= USB_HID_BURST);   /* рефлекс: остальное задержано */
        CHECK(strstr(logbuf, "types faster than a human"));
        usb_hid_verdict(0); usb_hid_poll(now);                                  /* Разум: отпустить - задержанное отдаётся */
        int later = 0; while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN) later++;
        CHECK(got + later == 30);
        for (int i = 0; i < 30; i++) key(&bad, 0x04);                          /* отпущенное устройство больше не задерживается */
        for (int i = 0; i < 10; i++) usb_hid_poll(now);
        devhealth_hid_take(&n, &rate, nm); CHECK(n == 0);
        while (input_pop(&ev)) {}
        /* второе устройство: Разум решает отключить, «usb allow» возвращает */
        mdev_kbd(&bad2); bad2.low_speed = 0;
        CHECK(plug_root(1, &bad2));
        unsigned hid0 = usb_hid_count();
        now += 1000;
        for (int i = 0; i < 40; i++) key(&bad2, 0x05);
        for (int i = 0; i < 12; i++) usb_hid_poll(now);
        devhealth_hid_take(&n, &rate, nm); CHECK(n == 1);
        usb_hid_verdict(1); usb_hid_poll(now);
        CHECK(usb_hid_count() == hid0 - 1 && strstr(logbuf, "disabled by the mind"));
        while (input_pop(&ev)) {}
        usb_hid_allow_all(); usb_hid_poll(now);
        CHECK(usb_hid_count() == hid0);
        for (int i = 0; i < 40; i++) key(&bad2, 0x05);
        for (int i = 0; i < 12; i++) usb_hid_poll(now);
        devhealth_hid_take(&n, &rate, nm); CHECK(n == 0);                        /* разрешённое не задерживается */
        got = 0; while (input_pop(&ev)) if (ev.type == INPUT_KEY_DOWN) got++;
        CHECK(got == 40);
    }
    printf("usb core: HID setup and reports, autorepeat, unplug with key held, HS hub + FS hub (route, TT, power, reset, change endpoint), "
           "nested keyboards, flash stick (BOT/SCSI, not-ready, STALL, reset recovery, MBR+FAT16 -> /usb0, 100 KB write/read, "
           "write-protect, no medium), hot unplug/replug, whole tree removal, keystroke-injection reflex (hold, release, block, allow) passed\n");
    return 0;
}
