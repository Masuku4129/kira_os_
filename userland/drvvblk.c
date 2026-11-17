/* Эталонный драйвер Ring 3 для диска virtio-blk (1af4:1042 и 1af4:1001, QEMU -device virtio-blk-pci): второй диск машины, который встроенный драйвер
   ядра не берёт (ядро пользуется одним диском virtio-blk). Образец канала CAP_BLKDEV (kdrv_blk.h): драйвер отдаёт ядру диск и обслуживает его кольца
   запросов без опроса: kdrv_irq_wait спит до прерывания устройства (MSI-X) или до звонка ядра (KDRV_EV_BLK).
   Транспорт - virtio 1.0 по PCI (capabilities, MMIO), очередь 0 (split ring). Данные читаются в буферы данных ядра и пишутся из них прямо по DMA
   (адрес даёт ядро в ctl->data_phys, под VT-d устройству открыты только эти буферы и память очереди этого драйвера).
   Аргументы запуска: пусто - обычная работа; «crash» - обратиться по нулевому адресу на первом запросе записи (ядро живо, запрос в полёте
   завершается ошибкой); «evil» - вести себя враждебно: head кольца ответов вне кольца и ответ с чужим номером (ядро отбрасывает и считает).
   Все запросы ядра проверяются теми же чистыми функциями (kdrv_blk.c), что и в ядре, прежде чем попасть к устройству. */
#include "kdrv.h"
#include "kdrv_blk.c"

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void put(i64 v) { char b[24]; int n = 0; u64 x = v < 0 ? (u64)-v : (u64)v; do { b[n++] = (char)('0' + x % 10); x /= 10; } while (x); if (v < 0) b[n++] = '-'; char o[26]; int k = 0; while (n) o[k++] = b[--n]; o[k] = 0; say(o); }
static void hex(u64 v) { char o[19]; int k = 0; o[k++] = '0'; o[k++] = 'x'; int started = 0; for (int s = 60; s >= 0; s -= 4) { int d = (int)((v >> s) & 15); if (d || started || s == 0) { o[k++] = "0123456789abcdef"[d]; started = 1; } } o[k] = 0; say(o); }

#define F_VERSION_1 (1ull << 32)
#define F_ACCESS_PLATFORM (1ull << 33)
#define F_RO (1ull << 5)
#define F_FLUSH (1ull << 9)
#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define ST_FEATURES_OK 8
#define ST_FAILED 0x80
#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4
#define E_PERM (-12)
#define E_ARG (-10)

/* common cfg */
#define C_DFSEL 0
#define C_DF 4
#define C_GFSEL 8
#define C_GF 12
#define C_STATUS 20
#define C_QSEL 22
#define C_QSIZE 24
#define C_QMSIX 26
#define C_QEN 28
#define C_QNOFF 30
#define C_QDESC 32
#define C_QAVAIL 40
#define C_QUSED 48

#define R8(p, o)  (*(volatile u8*)((volatile u8*)(p) + (o)))
#define R16(p, o) (*(volatile u16*)((volatile u8*)(p) + (o)))
#define R32(p, o) (*(volatile u32*)((volatile u8*)(p) + (o)))
static inline void barrier(void) { __asm__ volatile ("" ::: "memory"); __sync_synchronize(); }

static kdrv_info_t info;
static volatile u8* barmap[6];
static volatile u8 *common, *notify, *isr, *devcfg;
static u32 notify_mult;

static volatile u8* map_bar(u32 bar) {
    if (bar >= 6 || !info.bar[bar].size || (info.bar[bar].flags & KDRV_BAR_IO)) return 0;
    if (!barmap[bar]) { i64 e; barmap[bar] = (volatile u8*)kdrv_mmio_map(bar, 0, info.bar[bar].size, &e); }
    return barmap[bar];
}

/* Современный транспорт virtio: пройти по capabilities конфигурации (id 9) и отобразить области. */
static int find_caps(void) {
    i64 p = kdrv_cfg_read(0x34, 1);
    for (int guard = 0; p >= 0x40 && guard < 48; guard++) {
        p &= 0xFC;
        i64 id = kdrv_cfg_read((u32)p, 1), next = kdrv_cfg_read((u32)p + 1, 1);
        if (id < 0 || next < 0) return -1;
        if (id == 9) {
            i64 type = kdrv_cfg_read((u32)p + 3, 1), bar = kdrv_cfg_read((u32)p + 4, 1), off = kdrv_cfg_read((u32)p + 8, 4), len = kdrv_cfg_read((u32)p + 12, 4), clen = kdrv_cfg_read((u32)p + 2, 1);
            u64 need = type == 1 ? 56 : type == 2 ? 2 : 1;
            if (type >= 1 && type <= 4 && bar >= 0 && len >= (i64)need && (type != 2 || clen >= 20)) {
                volatile u8* m = map_bar((u32)bar);
                if (!m) return -2;
                m += off;
                if (type == 1 && !common) common = m;
                else if (type == 2 && !notify) { notify = m; notify_mult = (u32)kdrv_cfg_read((u32)p + 16, 4); }
                else if (type == 3 && !isr) isr = m;
                else if (type == 4 && !devcfg) devcfg = m;
            }
        }
        p = next;
    }
    return common && notify && isr && devcfg ? 0 : -3;
}

static void fail_dev(void) { R8(common, C_STATUS) = (u8)(R8(common, C_STATUS) | ST_FAILED); R8(common, C_STATUS) = 0; }

/* Очередь 0: дескрипторы по три на ячейку ядра (заголовок, данные, статус); память очереди и заголовки - один блок DMA. */
typedef struct { u64 addr; u32 len; u16 flags, next; } desc_t;
static desc_t* desc; static volatile u16* avail_ring; static volatile u16* used_ring;
static u16 qn, avail_idx, last_used;
static volatile u8* notify_addr;
static u64 qphys;
static u8* blk_hdrs; static volatile u8* blk_stat;                 /* заголовки запросов и байты статуса, по ячейкам */
static u64 hdr_phys, stat_phys;

static int queue_init(int want_msix) {
    R16(common, C_QSEL) = 0;
    u16 dmax = R16(common, C_QSIZE);
    if (!dmax || R16(common, C_QEN)) return -1;
    qn = dmax < 32 ? dmax : 32;
    while (qn & (qn - 1)) qn &= (u16)(qn - 1);
    if (qn < 4) return -1;
    kdrv_dma_t d;
    if (kdrv_dma_alloc(4096, 0, &d) != 0) return -2;
    desc = (desc_t*)d.va; qphys = d.phys;
    u32 avail_off = (u32)qn * 16, used_off = (avail_off + 6 + 2 * (u32)qn + 3) & ~3u;
    avail_ring = (volatile u16*)(d.va + avail_off); used_ring = (volatile u16*)(d.va + used_off);
    blk_hdrs = (u8*)(d.va + 1024); hdr_phys = d.phys + 1024;
    blk_stat = (volatile u8*)(d.va + 2048); stat_phys = d.phys + 2048;
    R16(common, C_QSIZE) = qn;
    R16(common, C_QMSIX) = want_msix ? 0 : 0xFFFF;                    /* запись 0 таблицы MSI-X: ядро настроило её при запуске драйвера */
    int msix = want_msix && R16(common, C_QMSIX) == 0;
    R32(common, C_QDESC) = (u32)d.phys; R32(common, C_QDESC + 4) = (u32)(d.phys >> 32);
    R32(common, C_QAVAIL) = (u32)(d.phys + avail_off); R32(common, C_QAVAIL + 4) = (u32)((d.phys + avail_off) >> 32);
    R32(common, C_QUSED) = (u32)(d.phys + used_off); R32(common, C_QUSED + 4) = (u32)((d.phys + used_off) >> 32);
    u16 noff = R16(common, C_QNOFF);
    R16(common, C_QEN) = 1;
    notify_addr = notify + (u64)noff * notify_mult;
    return msix;
}

static u64 features;
static int dev_begin(void) {
    R8(common, C_STATUS) = 0;
    for (int i = 0; i < 1000000 && R8(common, C_STATUS); i++) barrier();
    if (R8(common, C_STATUS)) return -1;
    R8(common, C_STATUS) = ST_ACK;
    R8(common, C_STATUS) = ST_ACK | ST_DRIVER;
    R32(common, C_DFSEL) = 0; u64 off = R32(common, C_DF);
    R32(common, C_DFSEL) = 1; off |= (u64)R32(common, C_DF) << 32;
    if (!(off & F_VERSION_1)) { fail_dev(); return -2; }
    features = F_VERSION_1 | (off & (F_ACCESS_PLATFORM | F_RO | F_FLUSH));        /* ACCESS_PLATFORM: DMA устройства идёт через IOMMU */
    R32(common, C_GFSEL) = 0; R32(common, C_GF) = (u32)features;
    R32(common, C_GFSEL) = 1; R32(common, C_GF) = (u32)(features >> 32);
    R8(common, C_STATUS) = ST_ACK | ST_DRIVER | ST_FEATURES_OK;
    if (!(R8(common, C_STATUS) & ST_FEATURES_OK)) { fail_dev(); return -3; }
    return 0;
}

/* Что ядро обязано отклонить при регистрации: каждый вызов - отказ KE_ARG (-10); без права CAP_BLKDEV - KE_PERM (-12) на всех. */
static int try_bad_registrations(void) {
    kdrv_blkreg_t r; u8* z;
    i64 res[8]; int n = 0;
    #define MK() do { z = (u8*)&r; for (unsigned i = 0; i < sizeof r; i++) z[i] = 0; r.name[0] = 'x'; r.name[1] = 'y'; r.sectors = 1024; r.sector_size = 512; } while (0)
    MK(); r.name[0] = 'X'; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);                     /* имя не из [a-z0-9] */
    MK(); r.sector_size = 4096; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);                /* сектор не 512 */
    MK(); r.sectors = 0; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);                       /* пустой диск */
    MK(); r.sectors = 1ull << 49; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);              /* ёмкость за пределом */
    MK(); r.flags = 0x80; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);                      /* неизвестные признаки */
    MK(); r.dma_bits = 5; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);                      /* предел DMA вне 20..63 */
    MK(); for (unsigned i = 0; i < sizeof r.name; i++) r.name[i] = 'a'; res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, (u64)&r, 0);   /* имя без нуля */
    res[n++] = ksys(KDRV_SYS_BLKDEV, KDRV_BLK_REGISTER, 0, 0);                                                  /* указатель на структуру - ноль */
    #undef MK
    if (res[0] == E_PERM) {
        int all = 1; for (int i = 0; i < n; i++) if (res[i] != E_PERM) all = 0;
        say(all ? "drvvblk: CAP_BLKDEV not granted: register denied (least-privilege PASS)\n" : "drvvblk: partial denial (FAIL)\n");
        return all ? 1 : -1;
    }
    int ok = 1; for (int i = 0; i < n; i++) if (res[i] != E_ARG) ok = 0;
    say("drvvblk: bad-reg: name="); put(res[0]); say(" sector="); put(res[1]); say(" empty="); put(res[2]); say(" huge="); put(res[3]);
    say(" flags="); put(res[4]); say(" bits="); put(res[5]); say(" noterm="); put(res[6]); say(" nullptr="); put(res[7]); say(ok ? " (refused PASS)\n" : " (FAIL)\n");
    return ok ? 0 : -1;
}

KIRA_ENTRY()
int umain(void) {
    if (kdrv_info(&info) != 0) { say("drvvblk: info refused (FAIL)\n"); return 1; }
    char args[16] = { 0 };
    k_args(args, sizeof args);
    int crash = args[0] == 'c', evil = args[0] == 'e';
    say("drvvblk: device "); hex(info.vendor); say(":"); hex(info.device); say(", rights "); hex(info.granted); say(info.flags & KDRV_INFO_MSI ? ", MSI-X\n" : ", no MSI\n");
    int br = try_bad_registrations();
    if (br != 0) return br > 0 ? 0 : 1;                                       /* без права диска - всё; с неверными отказами - ошибка */
    if (find_caps() != 0) { say("drvvblk: no virtio 1.0 capabilities (FAIL)\n"); return 1; }
    if (dev_begin() != 0) { say("drvvblk: feature negotiation failed (FAIL)\n"); return 1; }
    int msix = queue_init((info.flags & KDRV_INFO_MSI) != 0);
    if (msix < 0) { fail_dev(); say("drvvblk: queue setup failed (FAIL)\n"); return 1; }
    u64 cap = (u64)R32(devcfg, 0) | ((u64)R32(devcfg, 4) << 32);
    int inflight_max = (int)(qn / 3 < KDRV_BLK_SLOTS ? qn / 3 : KDRV_BLK_SLOTS);
    R8(common, C_STATUS) = (u8)(R8(common, C_STATUS) | ST_DRIVER_OK);
    say("drvvblk: capacity "); put((i64)cap); say(" sectors, queue "); put(qn); say(", features "); hex(features); say(msix ? ", completion by interrupt\n" : ", polling\n");

    kdrv_blkreg_t reg; u8* z = (u8*)&reg; for (unsigned i = 0; i < sizeof reg; i++) z[i] = 0;
    reg.name[0] = 'v'; reg.name[1] = 'd'; reg.name[2] = 'b';
    reg.sectors = cap; reg.sector_size = KDRV_BLK_SECTOR;
    reg.flags = ((features & F_RO) ? KDRV_BLK_F_RO : 0) | ((features & F_FLUSH) ? KDRV_BLK_F_FLUSH : 0);
    i64 err = 0;
    kdrv_blkctl_t* ctl = kdrv_blk_register(&reg, &err);
    if (!ctl) { say("drvvblk: register rc="); put(err); say(" (FAIL)\n"); fail_dev(); return 1; }
    say("drvvblk: disk vdb registered, data at "); hex(ctl->data_phys); say(", ring slots "); put(ctl->slots); say("\n");

    u32 req_tail = 0, rsp_head = 0;
    u8 busy[KDRV_BLK_SLOTS]; u32 rid[KDRV_BLK_SLOTS];
    for (unsigned i = 0; i < KDRV_BLK_SLOTS; i++) { busy[i] = 0; rid[i] = 0; }
    int inflight = 0; u32 completed = 0;
    if (evil) {                                                              /* head кольца ответов далеко вне кольца: ядро обязано пересинхронизироваться */
        rsp_head = 0x10000u; ctl->rsp_head = rsp_head;
        say("drvvblk: evil: rsp_head out of ring\n");
    }
    for (;;) {
        i64 ev = kdrv_irq_wait(msix ? 100 : 1);
        if (ev == KE_INTR) return 0;
        if (ev > 0 && (ev & KDRV_EV_IRQ)) (void)R8(isr, 0);                   /* чтение ISR снимает причину линии (для MSI-X безвредно) */
        int did = 0, notified = 0;
        barrier();
        while (last_used != used_ring[1]) {                                  /* ответы устройства */
            u32 e = last_used & (qn - 1);
            u32 head = ((volatile u32*)(used_ring + 2))[e * 2];
            last_used++;
            u32 slot = head / 3;
            if (slot >= KDRV_BLK_SLOTS || !busy[slot]) continue;
            int st = blk_stat[slot] == 0 ? 0 : -3;
            if (evil && completed == 2) {                                    /* ответ с чужим номером перед настоящим */
                volatile kdrv_blkrsp_t* f = &ctl->rsp[rsp_head % KDRV_BLK_SLOTS];
                f->id = 0xDEADBEEFu; f->status = 0; rsp_head++;
                say("drvvblk: evil: response with unknown id\n");
            }
            volatile kdrv_blkrsp_t* o = &ctl->rsp[rsp_head % KDRV_BLK_SLOTS];
            o->id = rid[slot]; o->status = st;
            rsp_head++; busy[slot] = 0; inflight--; completed++; did = 1;
        }
        for (;;) {                                                           /* новые запросы ядра */
            u32 head = __atomic_load_n(&ctl->req_head, __ATOMIC_ACQUIRE);
            int av = kdrv_blk_ring_avail(head, req_tail, KDRV_BLK_SLOTS);
            if (av < 0) { req_tail = head; ctl->req_tail = req_tail; break; }          /* кольцо испорчено: пересинхронизация */
            if (av == 0 || inflight >= inflight_max) break;
            kdrv_blkreq_t q = ctl->req[req_tail % KDRV_BLK_SLOTS];                      /* копия: проверяем то, что потом используем */
            req_tail++;
            __atomic_store_n(&ctl->req_tail, req_tail, __ATOMIC_RELEASE);
            int bad = kdrv_blk_req_check(&q, cap, reg.flags) != KDRV_BLK_OK || busy[q.slot % KDRV_BLK_SLOTS];
            if (!bad && q.op == KDRV_BLK_OP_FLUSH && !(reg.flags & KDRV_BLK_F_FLUSH)) bad = 1;
            if (bad) {                                                        /* ядро плохих запросов не шлёт; на всякий случай - ошибка, устройство не трогаем */
                volatile kdrv_blkrsp_t* o = &ctl->rsp[rsp_head % KDRV_BLK_SLOTS];
                o->id = q.id; o->status = -22; rsp_head++; did = 1;
                continue;
            }
            if (crash && q.op == KDRV_BLK_OP_WRITE) { say("drvvblk: crashing on purpose\n"); *(volatile u32*)0 = 1; }
            u32 s = q.slot;
            u32* h = (u32*)(blk_hdrs + s * 16);
            h[0] = q.op == KDRV_BLK_OP_READ ? T_IN : q.op == KDRV_BLK_OP_WRITE ? T_OUT : T_FLUSH; h[1] = 0; h[2] = (u32)q.lba; h[3] = (u32)(q.lba >> 32);
            blk_stat[s] = 0xFF;
            u32 d0 = s * 3;
            desc[d0].addr = hdr_phys + s * 16; desc[d0].len = 16; desc[d0].flags = 1; desc[d0].next = (u16)(d0 + 1);
            u32 last = d0 + 1;
            if (q.op != KDRV_BLK_OP_FLUSH) {
                desc[d0 + 1].addr = ctl->data_phys + (u64)s * KDRV_BLK_SLOT_BYTES; desc[d0 + 1].len = q.count * KDRV_BLK_SECTOR;
                desc[d0 + 1].flags = (u16)((q.op == KDRV_BLK_OP_READ ? 2 : 0) | 1); desc[d0 + 1].next = (u16)(d0 + 2);
                last = d0 + 2;
            }
            desc[last].addr = stat_phys + s; desc[last].len = 1; desc[last].flags = 2; desc[last].next = 0;
            avail_ring[2 + (avail_idx & (qn - 1))] = (u16)d0;
            barrier();
            avail_idx++; avail_ring[1] = avail_idx;
            barrier();
            busy[s] = 1; rid[s] = q.id; inflight++; notified = 1;
        }
        if (notified) *(volatile u16*)notify_addr = 0;                           /* очередь 0 */
        if (did) {
            __atomic_store_n(&ctl->rsp_head, rsp_head, __ATOMIC_RELEASE);
            kdrv_blk_kick();
        }
    }
}
