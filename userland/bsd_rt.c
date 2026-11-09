/* Среда исполнения слоя совместимости BSD (kcc/include/bsd/bsd_core.h) в драйвере Ring 3. Одна программа = один драйвер BSD: главный цикл
   опрашивает «прерывания» (INTx для Ring 3 нет), таймеры и кольцо передачи ядра; всё остальное - обычный код драйвера из корпуса OpenBSD.
   Склеивается с драйвером файлом-связкой (tools/bsd_glue.py): она даёт kd_ca (cfattach) и kd_devname. */
#include <bsd_core.h>

extern const struct cfattach* kd_ca;
extern const char* kd_devname;

/* ---- вывод ---- */
static void put_num(char** o, char* end, unsigned long long v, int base, int upper, int width, int zero, int neg) {
    char t[24]; int n = 0;
    do { int d = (int)(v % (unsigned)base); t[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= (unsigned)base; } while (v);
    if (neg) t[n++] = '-';
    while (n < width) t[n++] = zero ? '0' : ' ';
    while (n && *o < end) *(*o)++ = t[--n];
}
static int vfmt(char* buf, size_t cap, const char* f, va_list ap) {
    char* o = buf; char* end = buf + cap - 1;
    for (; *f && o < end; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++; int zero = 0, width = 0, lng = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z') { lng++; f++; }
        switch (*f) {
        case 'd': case 'i': { long long v = lng ? va_arg(ap, long) : va_arg(ap, int); put_num(&o, end, v < 0 ? (unsigned long long)-v : (unsigned long long)v, 10, 0, width, zero, v < 0); break; }
        case 'u': put_num(&o, end, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, zero, 0); break;
        case 'x': case 'X': put_num(&o, end, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, *f == 'X', width, zero, 0); break;
        case 'p': *o++ = '0'; if (o < end) *o++ = 'x'; put_num(&o, end, (unsigned long)va_arg(ap, void*), 16, 0, 0, 0, 0); break;
        case 'c': *o++ = (char)va_arg(ap, int); break;
        case 's': { const char* s = va_arg(ap, const char*); if (!s) s = "(null)"; int n = 0; while (s[n]) n++; while (n < width && o < end) { *o++ = ' '; width--; } while (*s && o < end) *o++ = *s++; break; }
        case '%': *o++ = '%'; break;
        case 0: f--; break;
        default: *o++ = '%'; if (o < end) *o++ = *f; break;
        }
    }
    *o = 0; return (int)(o - buf);
}
int snprintf(char* buf, size_t n, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfmt(buf, n, fmt, ap); va_end(ap); return r; }
void printf(const char* fmt, ...) { char b[256]; va_list ap; va_start(ap, fmt); int n = vfmt(b, sizeof b, fmt, ap); va_end(ap); k_write(b, (u64)n); }
void log(int level, const char* fmt, ...) { char b[256]; va_list ap; (void)level; va_start(ap, fmt); int n = vfmt(b, sizeof b, fmt, ap); va_end(ap); k_write(b, (u64)n); }
void panic(const char* fmt, ...) { char b[256]; va_list ap; k_puts("bsd panic: "); va_start(ap, fmt); int n = vfmt(b, sizeof b, fmt, ap); va_end(ap); k_write(b, (u64)n); k_puts("\n"); *(volatile int*)0 = 0; for (;;) { } }

/* ---- строки ---- */
void bcopy(const void* s, void* d, size_t n) { memcpy(d, s, n); }
void bzero(void* p, size_t n) { memset(p, 0, n); }
int bcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
size_t strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
size_t strlcpy(char* d, const char* s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; for (size_t i = 0; i < c; i++) d[i] = s[i]; d[c] = 0; } return l; }
/* Циклы с пустым asm: компилятор не превратит их обратно в вызов memcpy/memset (иначе бесконечная рекурсия). */
void* memcpy(void* d, const void* s, size_t n) { u8* a = (u8*)d; const u8* b = (const u8*)s; while (n--) { *a++ = *b++; __asm__ volatile ("" ::: "memory"); } return d; }
void* memset(void* d, int c, size_t n) { u8* a = (u8*)d; while (n--) { *a++ = (u8)c; __asm__ volatile ("" ::: "memory"); } return d; }
int memcmp(const void* x, const void* y, size_t n) { const u8* a = (const u8*)x; const u8* b = (const u8*)y; while (n--) { if (*a != *b) return *a - *b; a++; b++; } return 0; }

/* ---- память: классы по степеням двойки со свободными списками (mbuf выделяются и возвращаются постоянно) ---- */
#define NCLS 8                                   /* 32 .. 4096 */
struct mhdr { u32 cls; u32 magic; u64 pad; };    /* 16 байт: выравнивание данных */
static void* freelist[NCLS];
static u8* arena; static u64 arena_left;
void* malloc(size_t n, int type, int flags) {
    (void)type; (void)flags;
    size_t need = n + sizeof(struct mhdr); u32 c = 0; size_t sz = 32;
    while (sz < need && c < NCLS) { sz <<= 1; c++; }
    struct mhdr* h;
    if (c >= NCLS) {                             /* крупное: страницами напрямую, не возвращается */
        u64 pages = (need + 4095) / 4096; h = (struct mhdr*)k_heap_grow(pages); if (!h) return NULL; h->cls = NCLS; h->magic = 0x6b64u; return h + 1;
    }
    if (freelist[c]) { h = (struct mhdr*)freelist[c]; freelist[c] = *(void**)(h + 1); }
    else {
        if (arena_left < sz) { u64 pages = 16; arena = (u8*)k_heap_grow(pages); if (!arena) return NULL; arena_left = pages * 4096; }
        h = (struct mhdr*)arena; arena += sz; arena_left -= sz;
    }
    h->cls = c; h->magic = 0x6b64u;
    void* p = h + 1; memset(p, 0, sz - sizeof *h); return p;
}
void* mallocarray(size_t a, size_t b, int type, int flags) { return malloc(a * b, type, flags); }
void free(void* p, int type, size_t size) {
    (void)type; (void)size; if (!p) return;
    struct mhdr* h = (struct mhdr*)p - 1;
    if (h->magic != 0x6b64u) panic("free: bad block");
    if (h->cls >= NCLS) return;
    h->magic = 0; *(void**)p = freelist[h->cls]; freelist[h->cls] = h; h->magic = 0x6b64u;
}
void delay(unsigned us) { u64 t = k_clock_ns(); while (k_clock_ns() - t < (u64)us * 1000) { } }
static u64 now_ms(void) { return k_clock_ns() / 1000000; }

/* ---- mbuf ---- */
struct mbuf* m_gethdr(int how, int type) {
    (void)how; struct mbuf* m = (struct mbuf*)malloc(sizeof *m, 0, 0); if (!m) return NULL;
    m->m_data = m->m_buf; m->m_type = (short)type; m->m_flags = M_PKTHDR; return m;
}
struct mbuf* m_get(int how, int type) { struct mbuf* m = m_gethdr(how, type); if (m) m->m_flags = 0; return m; }
struct mbuf* m_clget(struct mbuf* m, int how, unsigned len) {
    (void)how; (void)len; char* c = (char*)malloc(MCLBYTES, 0, 0); if (!c) return m;
    m->m_ext.ext_buf = c; m->m_ext.ext_size = MCLBYTES; m->m_data = c; m->m_flags |= M_EXT; return m;
}
struct mbuf* m_free(struct mbuf* m) { struct mbuf* n = m->m_next; if (m->m_flags & M_EXT) free(m->m_ext.ext_buf, 0, 0); free(m, 0, 0); return n; }
void m_freem(struct mbuf* m) { while (m) m = m_free(m); }
void ml_enqueue(struct mbuf_list* ml, struct mbuf* m) { m->m_nextpkt = NULL; if (ml->ml_tail) ml->ml_tail->m_nextpkt = m; else ml->ml_head = m; ml->ml_tail = m; ml->ml_len++; }
struct mbuf* ml_dequeue(struct mbuf_list* ml) { struct mbuf* m = ml->ml_head; if (!m) return NULL; ml->ml_head = m->m_nextpkt; if (!ml->ml_head) ml->ml_tail = NULL; m->m_nextpkt = NULL; ml->ml_len--; return m; }
struct mbuf* ifq_dequeue(struct ifqueue* q) { struct mbuf* m = q->ifq_head; if (!m) return NULL; q->ifq_head = m->m_nextpkt; if (!q->ifq_head) q->ifq_tail = NULL; m->m_nextpkt = NULL; q->ifq_len--; return m; }
static void ifq_enqueue(struct ifqueue* q, struct mbuf* m) { m->m_nextpkt = NULL; if (q->ifq_tail) q->ifq_tail->m_nextpkt = m; else q->ifq_head = m; q->ifq_tail = m; q->ifq_len++; }

/* ---- ifmedia ---- */
void ifmedia_init(struct ifmedia* m, int mask, int (*chg)(struct ifnet*), void (*st)(struct ifnet*, struct ifmediareq*)) { bzero(m, sizeof *m); m->ifm_mask = mask; m->ifm_change_cb = chg; m->ifm_status_cb = st; }
void ifmedia_add(struct ifmedia* m, int word, int data, void* aux) {
    if (m->ifm_n >= IFM_MAXENT) { printf("ifmedia_add: table full\n"); return; }
    m->ifm_ent[m->ifm_n].ifm_media = word; m->ifm_ent[m->ifm_n].ifm_data = data; m->ifm_ent[m->ifm_n].ifm_aux = aux; m->ifm_n++;
}
void ifmedia_set(struct ifmedia* m, int word) {
    for (int i = 0; i < m->ifm_n; i++) if (m->ifm_ent[i].ifm_media == word) { m->ifm_cur = &m->ifm_ent[i]; m->ifm_media = word; return; }
    panic("ifmedia_set: no match for 0x%x", word);
}
int ifmedia_ioctl(struct ifnet* ifp, struct ifreq* ifr, struct ifmedia* m, u_long cmd) {
    if (cmd == SIOCGIFMEDIA) {
        struct ifmediareq* r = &ifr->ifr_media; r->ifm_status = 0; r->ifm_active = m->ifm_cur ? m->ifm_cur->ifm_media : 0;
        r->ifm_current = r->ifm_active; r->ifm_count = m->ifm_n; if (m->ifm_status_cb) m->ifm_status_cb(ifp, r); return 0;
    }
    if (cmd == SIOCSIFMEDIA) {
        int w = ifr->ifr_media.ifm_current;
        for (int i = 0; i < m->ifm_n; i++) if (m->ifm_ent[i].ifm_media == w) { m->ifm_cur = &m->ifm_ent[i]; m->ifm_media = w; return m->ifm_change_cb ? m->ifm_change_cb(ifp) : 0; }
        return EINVAL;
    }
    return EINVAL;
}
void ifmedia_delete_instance(struct ifmedia* m, unsigned inst) { (void)inst; m->ifm_n = 0; m->ifm_cur = NULL; }

static kdrv_info_t g_info;                                    /* сведения ядра об устройстве (kdrv_info) */

/* ---- bus_dma ----
   Блоки DMA ядра (kdrv_dma_alloc) - единственная память, видимая устройству (при VT-d - только она). Реестр блоков нужен, чтобы по виртуальному адресу
   найти физический. Mbuf и чужие буферы лежат вне блоков: их содержимое копируется через «промежуточный» кусок арены при bus_dmamap_sync. */
#define KD_ARENA_CHUNK (256u * 1024)
static struct { u64 va, phys, bytes; } kd_blk[KDRV_MAX_DMA]; static int kd_nblk;
static u64 kd_arena_va, kd_arena_phys, kd_arena_left; static struct kd_dmat kd_dmat0 = { 32 };
bus_dma_tag_t kd_dmat(void) { return &kd_dmat0; }
static int kd_blk_new(u64 bytes, int bits, kdrv_dma_t* d) {
    if (kd_nblk >= KDRV_MAX_DMA || kdrv_dma_alloc(bytes, (u32)bits, d) != 0) return -1;
    kd_blk[kd_nblk].va = d->va; kd_blk[kd_nblk].phys = d->phys; kd_blk[kd_nblk].bytes = d->bytes; kd_nblk++; return 0;
}
static int kd_blk_find(const void* p, u64* phys) {
    u64 a = (u64)(unsigned long)p;
    for (int i = 0; i < kd_nblk; i++) if (a >= kd_blk[i].va && a < kd_blk[i].va + kd_blk[i].bytes) { *phys = kd_blk[i].phys + (a - kd_blk[i].va); return 1; }
    return 0;
}
static void* kd_arena_get(bus_dma_tag_t t, u64 bytes, u64* phys) {
    bytes = (bytes + 63) & ~63ull;
    if (bytes > kd_arena_left) {
        kdrv_dma_t d; if (bytes > KD_ARENA_CHUNK / 2 || kd_blk_new(KD_ARENA_CHUNK, t->bits, &d) != 0) return NULL;
        kd_arena_va = d.va; kd_arena_phys = d.phys; kd_arena_left = d.bytes;
    }
    void* p = (void*)(unsigned long)kd_arena_va; *phys = kd_arena_phys;
    kd_arena_va += bytes; kd_arena_phys += bytes; kd_arena_left -= bytes; return p;
}
int bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs, bus_size_t maxsegsz, bus_size_t boundary, int flags, bus_dmamap_t* mp) {
    (void)t; (void)maxsegsz; (void)boundary; (void)flags;
    struct kd_dmamap* m = malloc(sizeof *m, 0, 0); if (!m) return ENOMEM;
    bzero(m, sizeof *m); m->_max = size; m->_maxsegs = nsegs > KD_DMA_MAXSEG ? KD_DMA_MAXSEG : nsegs; *mp = m; return 0;
}
void bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t m) { (void)t; free(m, 0, 0); }   /* промежуточный кусок арены остаётся (до конца драйвера) */
static int kd_load_common(bus_dma_tag_t t, bus_dmamap_t m, bus_size_t len) {
    if (len > m->_max) return EFBIG;
    if (!m->_bva) { u64 ph; m->_bva = kd_arena_get(t, m->_max, &ph); if (!m->_bva) return ENOMEM; m->_bphys = (unsigned long)ph; m->_bcap = m->_max; }
    m->dm_mapsize = len; m->dm_nsegs = 1; m->dm_segs[0].ds_addr = m->_bphys; m->dm_segs[0].ds_len = len; m->dm_segs[0]._ds_va = m->_bva; m->_direct = 0; return 0;
}
int bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t m, void* buf, bus_size_t len, struct proc* p, int flags) {
    (void)p; (void)flags; u64 ph;
    m->_um = NULL; m->_uva = buf;
    if (kd_blk_find(buf, &ph) && kd_blk_find((char*)buf + (len ? len - 1 : 0), &ph)) {        /* память DMA: устройству отдаётся как есть */
        kd_blk_find(buf, &ph);
        m->dm_mapsize = len; m->dm_nsegs = 1; m->dm_segs[0].ds_addr = (unsigned long)ph; m->dm_segs[0].ds_len = len; m->dm_segs[0]._ds_va = buf; m->_direct = 1; return 0;
    }
    return kd_load_common(t, m, len);
}
int bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t m, struct mbuf* mb, int flags) {
    (void)flags; bus_size_t len = 0; for (struct mbuf* p = mb; p; p = p->m_next) len += (bus_size_t)p->m_len;
    m->_um = mb; m->_uva = NULL; return kd_load_common(t, m, len);
}
void bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t m) { (void)t; m->dm_mapsize = 0; m->dm_nsegs = 0; m->_um = NULL; m->_uva = NULL; m->_direct = 0; }
static void kd_mb_copy(struct mbuf* mb, bus_size_t off, bus_size_t len, void* bounce, int to_mbuf) {     /* цепочка mbuf <-> непрерывный буфер, диапазон [off, off+len) */
    bus_size_t pos = 0; char* b = bounce;
    for (struct mbuf* p = mb; p && len; p = p->m_next) {
        bus_size_t pl = (bus_size_t)p->m_len; if (off >= pos + pl) { pos += pl; continue; }
        bus_size_t s = off > pos ? off - pos : 0, n = pl - s; if (n > len) n = len;
        char* mem = (char*)p->m_data + s; char* bo = b + pos + s;
        if (to_mbuf) memcpy(mem, bo, n); else memcpy(bo, mem, n);
        len -= n; off += n; pos += pl;
    }
}
void bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t m, bus_addr_t off, bus_size_t len, int ops) {
    (void)t; __sync_synchronize();
    if (m->_direct || !m->_bva || !m->dm_nsegs) return;
    if (off > m->dm_mapsize) return; if (len > m->dm_mapsize - off) len = m->dm_mapsize - off;
    if (ops & BUS_DMASYNC_PREWRITE) { if (m->_um) kd_mb_copy(m->_um, off, len, m->_bva, 0); else if (m->_uva) memcpy((char*)m->_bva + off, (char*)m->_uva + off, len); }
    if (ops & BUS_DMASYNC_POSTREAD) { if (m->_um) kd_mb_copy(m->_um, off, len, m->_bva, 1); else if (m->_uva) memcpy((char*)m->_uva + off, (char*)m->_bva + off, len); }
    __sync_synchronize();
}
int bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary, bus_dma_segment_t* segs, int nsegs, int* rsegs, int flags) {
    (void)align; (void)boundary; (void)flags; kdrv_dma_t d;
    if (nsegs < 1 || size > KDRV_MAX_DMA_BYTES || kd_blk_new(size, t->bits, &d) != 0) return ENOMEM;   /* блок выровнен по странице и непрерывен */
    segs[0].ds_addr = (unsigned long)d.phys; segs[0].ds_len = d.bytes; segs[0]._ds_va = (void*)(unsigned long)d.va; *rsegs = 1; return 0;
}
int bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t* segs, int nsegs, size_t size, caddr_t* kvap, int flags) { (void)t; (void)nsegs; (void)size; (void)flags; *kvap = segs[0]._ds_va; return 0; }
void bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size) { (void)t; (void)kva; (void)size; }
void bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t* segs, int nsegs) { (void)t; (void)segs; (void)nsegs; }   /* ядро освобождает блоки при гибели драйвера */
int pci_matchid(struct pci_attach_args* pa, const struct pci_matchid* ids, int n) {
    for (int i = 0; i < n; i++) if (PCI_VENDOR(pa->pa_id) == ids[i].pm_vid && PCI_PRODUCT(pa->pa_id) == ids[i].pm_pid) return 1;
    return 0;
}

/* ---- MII: один общий PHY IEEE 802.3 (как ukphy в OpenBSD). Находит первый отвечающий адрес, автосогласование, состояние связи. Особенностей
   конкретных PHY (отдельные драйверы dev/mii/*phy.c) здесь нет: с ними связь и скорость определяются только стандартными регистрами. ---- */
#define MR(m, r) ((m)->mii_readreg((m)->mii_dev, ((struct mii_softc*)LIST_FIRST(&(m)->mii_phys))->mii_phy, (r)))
#define MW(m, r, v) ((m)->mii_writereg((m)->mii_dev, ((struct mii_softc*)LIST_FIRST(&(m)->mii_phys))->mii_phy, (r), (v)))
void mii_attach(struct device* self, struct mii_data* mii, int capmask, int phyloc, int offloc, int flags) {
    (void)offloc; (void)flags; mii->mii_dev = self; LIST_INIT(&mii->mii_phys);
    for (int ph = (phyloc == MII_PHY_ANY ? 0 : phyloc); ph < (phyloc == MII_PHY_ANY ? 32 : phyloc + 1); ph++) {
        int bmsr = mii->mii_readreg(self, ph, MII_BMSR), id1 = mii->mii_readreg(self, ph, MII_PHYIDR1), id2 = mii->mii_readreg(self, ph, MII_PHYIDR2);
        if (bmsr == 0 || bmsr == 0xffff || (id1 == 0xffff && id2 == 0xffff) || (id1 == 0 && id2 == 0)) continue;
        struct mii_softc* sc = malloc(sizeof *sc, 0, 0); if (!sc) return;
        bzero(sc, sizeof *sc); sc->mii_phy = ph; sc->mii_pdata = mii; sc->mii_capabilities = bmsr & capmask & 0xffff;
        LIST_INSERT_HEAD(&mii->mii_phys, sc, mii_list);
        mii->mii_writereg(self, ph, MII_BMCR, BMCR_RESET);
        for (int i = 0; i < 500 && (mii->mii_readreg(self, ph, MII_BMCR) & BMCR_RESET); i++) delay(1000);
        int c = sc->mii_capabilities;
        if (c & BMSR_10THDX) ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_10_T, 0, NULL);
        if (c & BMSR_10TFDX) ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_10_T | IFM_FDX, 0, NULL);
        if (c & BMSR_100TXHDX) ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_100_TX, 0, NULL);
        if (c & BMSR_100TXFDX) ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_100_TX | IFM_FDX, 0, NULL);
        if (c & BMSR_ANEG) ifmedia_add(&mii->mii_media, IFM_ETHER | IFM_AUTO, 0, NULL);
        printf("%s: PHY %d, id %04x:%04x (общий драйвер IEEE 802.3)\n", self->dv_xname, ph, id1, id2);
        return;                                                        /* первый найденный PHY; несколько на одной шине не поддержаны */
    }
}
int mii_mediachg(struct mii_data* mii) {
    if (LIST_EMPTY(&mii->mii_phys) || !mii->mii_media.ifm_cur) return 0;
    int w = mii->mii_media.ifm_cur->ifm_media, st = IFM_SUBTYPE(w);
    struct mii_softc* sc = LIST_FIRST(&mii->mii_phys); int c = sc->mii_capabilities;
    if (st == IFM_AUTO) {
        int anar = ANAR_CSMA | ((c & BMSR_10THDX) ? ANAR_10 : 0) | ((c & BMSR_10TFDX) ? ANAR_10_FD : 0) | ((c & BMSR_100TXHDX) ? ANAR_TX : 0) | ((c & BMSR_100TXFDX) ? ANAR_TX_FD : 0);
        MW(mii, MII_ANAR, anar); MW(mii, MII_BMCR, BMCR_AUTOEN | BMCR_STARTNEG);
    } else MW(mii, MII_BMCR, (st == IFM_100_TX ? BMCR_S100 : 0) | ((w & IFM_FDX) ? BMCR_FDX : 0));
    mii_pollstat(mii); return 0;
}
void mii_pollstat(struct mii_data* mii) {
    if (LIST_EMPTY(&mii->mii_phys)) { mii->mii_media_status = IFM_AVALID; mii->mii_media_active = IFM_ETHER | IFM_NONE; return; }
    int old = mii->mii_media_active, oldst = mii->mii_media_status;
    (void)MR(mii, MII_BMSR); int bmsr = MR(mii, MII_BMSR), bmcr = MR(mii, MII_BMCR);      /* состояние связи «залипает»: читать дважды */
    mii->mii_media_status = IFM_AVALID; mii->mii_media_active = IFM_ETHER;
    if (!(bmsr & BMSR_LINK)) mii->mii_media_active |= IFM_NONE;
    else {
        mii->mii_media_status |= IFM_ACTIVE;
        if (bmcr & BMCR_AUTOEN) {
            if (!(bmsr & BMSR_ACOMP)) mii->mii_media_active |= IFM_NONE;
            else { int a = MR(mii, MII_ANAR) & MR(mii, MII_ANLPAR);
                if (a & ANAR_TX_FD) mii->mii_media_active |= IFM_100_TX | IFM_FDX; else if (a & ANAR_TX) mii->mii_media_active |= IFM_100_TX;
                else if (a & ANAR_10_FD) mii->mii_media_active |= IFM_10_T | IFM_FDX; else mii->mii_media_active |= IFM_10_T; }
        } else mii->mii_media_active |= ((bmcr & BMCR_S100) ? IFM_100_TX : IFM_10_T) | ((bmcr & BMCR_FDX) ? IFM_FDX : 0);
    }
    if ((old != mii->mii_media_active || oldst != mii->mii_media_status) && mii->mii_statchg) mii->mii_statchg(mii->mii_dev);
}
void mii_tick(struct mii_data* mii) { mii_pollstat(mii); }
void mii_down(struct mii_data* mii) { (void)mii; }

/* ---- ether ---- */
char* ether_sprintf(u_int8_t* a) { static char b[18]; snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]); return b; }
u_int32_t ether_crc32_be(const u_int8_t* p, size_t n) {
    u_int32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) { u_int8_t c = p[i]; for (int b = 0; b < 8; b++) { u_int32_t carry = ((crc >> 31) & 1) ^ (c & 1); crc <<= 1; c >>= 1; if (carry) crc = (crc ^ 0x04c11db7u); } }
    return crc;
}
u_int32_t ether_crc32_le(const u_int8_t* p, size_t n) {
    u_int32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) { crc ^= p[i]; for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0); }
    return crc;
}
int pci_matchbyid(struct pci_attach_args* pa, const struct pci_matchid* ids, int n) { return pci_matchid(pa, ids, n); }
/* состояние питания через возможность PM (id 1, PMCSR = +4, биты 0..1); устройству нужно время на выход из D3 */
int pci_set_powerstate(pci_chipset_tag_t pc, pcitag_t tag, int state) {
    (void)pc; (void)tag;
    if (!(pci_conf_read(NULL, 0, 0x04) & 0x00100000)) return 0;
    unsigned ptr = pci_conf_read(NULL, 0, 0x34) & 0xfc;
    for (int guard = 0; ptr >= 0x40 && guard < 48; guard++) {
        u32 h = pci_conf_read(NULL, 0, (int)ptr);
        if ((h & 0xff) == 1) {
            u32 r = pci_conf_read(NULL, 0, (int)ptr + 4);
            if ((r & 3) != (u32)state) { pci_conf_write(NULL, 0, (int)ptr + 4, (r & ~3u) | (u32)state); delay(10000); }
            return 0;
        }
        ptr = (h >> 8) & 0xfc;
    }
    return 0;
}
int m_defrag(struct mbuf* m, int how) {
    if (!m->m_next) return 0;
    int len = 0; for (struct mbuf* p = m; p; p = p->m_next) len += p->m_len;
    if (len > MCLBYTES) return ENOBUFS;
    char* c = malloc(MCLBYTES, 0, 0); if (!c) return ENOBUFS; (void)how;
    int o = 0; for (struct mbuf* p = m; p; p = p->m_next) { memcpy(c + o, p->m_data, (size_t)p->m_len); o += p->m_len; }
    if (m->m_next) { m_freem(m->m_next); m->m_next = NULL; }
    if (m->m_flags & M_EXT) free(m->m_ext.ext_buf, 0, 0);
    m->m_ext.ext_buf = c; m->m_ext.ext_size = MCLBYTES; m->m_flags |= M_EXT; m->m_data = c; m->m_len = len;
    if (m->m_pkthdr.len < len) m->m_pkthdr.len = len;
    return 0;
}
int ether_ioctl(struct ifnet* ifp, struct arpcom* ac, u_long cmd, caddr_t data) { (void)ifp; (void)ac; (void)cmd; (void)data; return ENOTTY; }

static struct ifnet* g_ifp;
void if_attach(struct ifnet* ifp) { ifp->if_index = 1; ifp->if_snd.ifq_maxlen = 8; g_ifp = ifp; }
void if_detach(struct ifnet* ifp) { if (g_ifp == ifp) g_ifp = NULL; }
void ether_ifattach(struct ifnet* ifp) {
    struct arpcom* ac = (struct arpcom*)ifp;                    /* ac_if - первый член */
    ifp->if_sadl = (struct sockaddr_dl*)ifp->if_sadl_buf; for (int i = 0; i < 6; i++) ifp->if_sadl_buf[i] = ac->ac_enaddr[i];
    ac->ac_multirangecnt = 1;                                   /* список групп ядра не передаётся: принимать все групповые (нужно IPv6) */
    ifp->if_ctl = kdrv_net_register(ac->ac_enaddr);
    if (!ifp->if_ctl) printf("%s: ядро отказало в регистрации сетевой карты\n", ifp->if_xname);
}
void ether_ifdetach(struct ifnet* ifp) { (void)ifp; }

/* принятые кадры -> кольцо приёма ядра */
void if_input(struct ifnet* ifp, struct mbuf_list* ml) {
    kdrv_netctl_t* c = ifp->if_ctl; struct mbuf* m; int any = 0;
    while ((m = ml_dequeue(ml)) != NULL) {
        u32 len = 0; for (struct mbuf* p = m; p; p = p->m_next) len += (u32)p->m_len;
        u32 head = c ? c->rx_head : 0, tail = c ? __atomic_load_n(&c->rx_tail, __ATOMIC_ACQUIRE) : 0;
        if (!c || len < ETHER_HDR_LEN || len > c->max_frame || head - tail >= c->slots) { ifp->if_iqdrops++; printf("bsdrt: rx drop len %u pkthdr %d slots %d\n", len, m->m_pkthdr.len, c ? (int)(head - tail) : -1); m_freem(m); continue; }
        kdrv_slot_t* s = (kdrv_slot_t*)((u8*)c + KDRV_NET_RX_OFF) + (head % c->slots);
        u32 off = 0; for (struct mbuf* p = m; p; p = p->m_next) { memcpy(s->data + off, p->m_data, (size_t)p->m_len); off += (u32)p->m_len; }
        s->len = len; __atomic_store_n(&c->rx_head, head + 1, __ATOMIC_RELEASE);
        ifp->if_ipackets++; any = 1; m_freem(m);
    }
    if (any) kdrv_net_kick();
}

/* ---- bus_space ---- */
static void io_fail(i64 r) { static int once; if (!once) { once = 1; printf("bsd: порты отказали: %d\n", (int)r); } }
u_int32_t kd_bs_read(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w) {
    /* AMD PCnet в 32-битном режиме (в него драйвер переводит чип первым делом): QEMU отдаёт APROM (MAC) только двойными словами, байтовое и 16-битное
       чтение возвращают ff. Байт APROM читается как dword по выровненному смещению; на настоящих платах такое чтение APROM тоже допустимо. */
    if (w == 1 && o < 16 && g_info.vendor == 0x1022 && g_info.device == 0x2000) {
        bus_size_t a = o & ~(bus_size_t)3; u32 v;
        if (t->io) { i64 r = kdrv_io_in((u32)t->bar, (u32)(h + a), 4); if (r < 0) { io_fail(r); return 0xff; } v = (u32)r; }
        else v = *(volatile u32*)(h + a);
        return (v >> (8 * (o & 3))) & 0xff;
    }
    if (t->io) { i64 r = kdrv_io_in((u32)t->bar, (u32)(h + o), (u32)w); if (r < 0) { io_fail(r); return 0xffffffffu >> (32 - 8 * w); } return (u32)r; }
    volatile u8* p = (volatile u8*)(h + o);
    return w == 1 ? *p : w == 2 ? *(volatile u16*)p : *(volatile u32*)p;
}
void kd_bs_write(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, u_int32_t v) {
    if (t->io) { i64 r = kdrv_io_out((u32)t->bar, (u32)(h + o), (u32)w, v); if (r < 0) io_fail(r); return; }
    volatile u8* p = (volatile u8*)(h + o);
    if (w == 1) *p = (u8)v; else if (w == 2) *(volatile u16*)p = (u16)v; else *(volatile u32*)p = v;
}
void kd_bs_read_multi(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, void* buf, size_t bytes) {
    u8* b = (u8*)buf;
    if (t->io) { while (bytes) { size_t c = bytes > KDRV_IO_MAX_BYTES ? KDRV_IO_MAX_BYTES : bytes; i64 r = kdrv_io_ins((u32)t->bar, (u32)(h + o), (u32)w, b, (u32)(c / (size_t)w)); if (r < 0) { io_fail(r); return; } b += c; bytes -= c; } return; }
    for (size_t i = 0; i < bytes; i += (size_t)w) { u32 v = kd_bs_read(t, h, o, w); for (int k = 0; k < w; k++) b[i + (size_t)k] = (u8)(v >> (8 * k)); }
}
void kd_bs_write_multi(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, const void* buf, size_t bytes) {
    const u8* b = (const u8*)buf;
    if (t->io) { while (bytes) { size_t c = bytes > KDRV_IO_MAX_BYTES ? KDRV_IO_MAX_BYTES : bytes; i64 r = kdrv_io_outs((u32)t->bar, (u32)(h + o), (u32)w, b, (u32)(c / (size_t)w)); if (r < 0) { io_fail(r); return; } b += c; bytes -= c; } return; }
    for (size_t i = 0; i < bytes; i += (size_t)w) { u32 v = 0; for (int k = 0; k < w; k++) v |= (u32)b[i + (size_t)k] << (8 * k); kd_bs_write(t, h, o, w, v); }
}
void bus_space_read_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int8_t* p, bus_size_t n) { for (bus_size_t i = 0; i < n; i++) p[i] = bus_space_read_1(t, h, o + i); }
void bus_space_write_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, const u_int8_t* p, bus_size_t n) { for (bus_size_t i = 0; i < n; i++) bus_space_write_1(t, h, o + i, p[i]); }
void bus_space_set_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int8_t v, bus_size_t n) { for (bus_size_t i = 0; i < n; i++) bus_space_write_1(t, h, o + i, v); }
int bus_space_subregion(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, bus_size_t size, bus_space_handle_t* nh) { (void)t; (void)size; *nh = h + o; return 0; }
void bus_space_unmap(bus_space_tag_t t, bus_space_handle_t h, bus_size_t size) { (void)t; (void)h; (void)size; }   /* отображения и порты возвращает ядро при остановке драйвера */

/* ---- PCI ---- */
static struct kd_bst bst_pool[6]; static int bst_n;
int pci_mapreg_map(struct pci_attach_args* pa, int reg, pcireg_t type, int flags, bus_space_tag_t* tp, bus_space_handle_t* hp, bus_addr_t* basep, bus_size_t* sizep, bus_size_t maxsize) {
    (void)pa; (void)flags; (void)maxsize;
    if (reg < PCI_MAPREG_START || reg > PCI_MAPREG_START + 20 || (reg & 3)) return EINVAL;
    int bar = (reg - PCI_MAPREG_START) / 4; u64 size = g_info.bar[bar].size;
    if (!size || bst_n >= 6) return ENOENT;
    int io = (g_info.bar[bar].flags & KDRV_BAR_IO) != 0;
    if (io != (type == PCI_MAPREG_TYPE_IO)) return EINVAL;
    struct kd_bst* t = &bst_pool[bst_n++]; t->bar = bar; t->io = io;
    if (io) *hp = 0;
    else { i64 err = 0; volatile void* m = kdrv_mmio_map((u32)bar, 0, size, &err); if (!m) return ENOMEM; *hp = (bus_space_handle_t)m; }
    *tp = t; if (basep) *basep = 0; if (sizep) *sizep = size; return 0;
}
pcireg_t pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg) { (void)pc; (void)tag; i64 r = kdrv_cfg_read((u32)reg, 4); return r < 0 ? 0xffffffffu : (u32)r; }
void pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t v) { (void)pc; (void)tag; kdrv_cfg_write((u32)reg, 4, v); }
/* Прерывание драйвера BSD: если ядро выдало устройству прерывание (INTx или MSI, права irq), обработчик вызывается из главного цикла по событию
   KDRV_EV_IRQ; иначе (права irq нет) - опрос каждые 10 мс. Какой режим - видно в pci_intr_string. */
#define NIH 4
static struct { int (*fn)(void*); void* arg; } ihs[NIH]; static int nih;
int pci_intr_map(struct pci_attach_args* pa, pci_intr_handle_t* ih) { (void)pa; *ih = 0; return 0; }
const char* pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih) { (void)pc; (void)ih; return (g_info.flags & KDRV_INFO_INTX) ? "intx" : (g_info.flags & KDRV_INFO_MSI) ? "msi" : "polling"; }
void* pci_intr_establish(pci_chipset_tag_t pc, pci_intr_handle_t ih, int ipl, int (*fn)(void*), void* arg, const char* name) {
    (void)pc; (void)ih; (void)ipl; (void)name; if (nih >= NIH) return NULL; ihs[nih].fn = fn; ihs[nih].arg = arg; return &ihs[nih++];
}
void pci_intr_disestablish(pci_chipset_tag_t pc, void* h) { (void)pc; if (h) ((struct { int (*fn)(void*); void* arg; }*)h)->fn = NULL; }

/* ---- timeout ---- */
#define NTO 16
static struct timeout* tos[NTO];
void timeout_set(struct timeout* t, void (*fn)(void*), void* arg) { t->to_fn = fn; t->to_arg = arg; t->to_set = 0; }
void timeout_add_msec(struct timeout* t, int ms) {
    t->to_due = (unsigned long)now_ms() + (unsigned long)ms; t->to_set = 1;
    for (int i = 0; i < NTO; i++) if (tos[i] == t) return;
    for (int i = 0; i < NTO; i++) if (!tos[i]) { tos[i] = t; return; }
    panic("timeout table full");
}
int timeout_del(struct timeout* t) { int was = t->to_set; t->to_set = 0; return was; }

/* ---- главный цикл ---- */
KIRA_ENTRY()
int umain(void) {
    if (kdrv_info(&g_info) != 0) { printf("bsdrt: info refused\n"); return 1; }
    struct pci_attach_args pa; bzero(&pa, sizeof pa);
    pa.pa_dmat = kd_dmat(); pa.pa_id = (u32)g_info.vendor | ((u32)g_info.device << 16); pa.pa_class = g_info.cls << 8;
    struct device* dev = (struct device*)malloc(kd_ca->ca_devsize, M_DEVBUF, M_ZERO);
    if (!dev) { printf("bsdrt: no memory for softc\n"); return 1; }
    snprintf(dev->dv_xname, sizeof dev->dv_xname, "%s0", kd_devname);
    if (!kd_ca->ca_match(NULL, NULL, &pa)) { printf("bsdrt: %s: match refused %04x:%04x\n", dev->dv_xname, g_info.vendor, g_info.device); return 1; }
    printf("%s at pci %04x:%04x", dev->dv_xname, g_info.vendor, g_info.device);
    kd_ca->ca_attach(NULL, dev, &pa);
    struct ifnet* ifp = g_ifp;
    if (!ifp || !ifp->if_ctl) { printf("bsdrt: %s: сетевая карта не зарегистрирована (FAIL)\n", dev->dv_xname); return 1; }
    ifp->if_flags |= IFF_UP;
    if (ifp->if_ioctl) ifp->if_ioctl(ifp, SIOCSIFFLAGS, NULL);                  /* то же, что ifconfig up: инициализация карты */
    __atomic_store_n(&ifp->if_ctl->link, 1u, __ATOMIC_RELEASE);
    printf("bsdrt: %s up, mac %s\n", ifp->if_xname, ether_sprintf(((struct arpcom*)ifp)->ac_enaddr));
    int use_irq = (g_info.flags & (KDRV_INFO_INTX | KDRV_INFO_MSI)) != 0;
    printf("bsdrt: прерывания: %s\n", use_irq ? ((g_info.flags & KDRV_INFO_INTX) ? "INTx по событию" : "MSI по событию") : "опрос раз в 10 мс");
    unsigned irqs = 0; i64 ev = 0;
    u64 next_sec = now_ms() + 1000;
    for (;;) {
        if (!use_irq || (ev & KDRV_EV_IRQ)) { if (use_irq) irqs++; for (int i = 0; i < nih; i++) if (ihs[i].fn) ihs[i].fn(ihs[i].arg); }
        u64 now = now_ms();
        for (int i = 0; i < NTO; i++) if (tos[i] && tos[i]->to_set && (long)((unsigned long)now - tos[i]->to_due) >= 0) { tos[i]->to_set = 0; tos[i]->to_fn(tos[i]->to_arg); }
        if (now >= next_sec) { next_sec += 1000;
            static unsigned lo, li, le, lt; static int tick;
            if (++tick >= 5 && (ifp->if_opackets != lo || ifp->if_ipackets != li || ifp->if_ierrors != le || ifp->if_oerrors != lt)) {   /* сводка раз в 5 с, только при изменении */
                tick = 0; lo = ifp->if_opackets; li = ifp->if_ipackets; le = ifp->if_ierrors; lt = ifp->if_oerrors;
                printf("bsdrt: %s: tx %u rx %u rx-err %u tx-err %u rx-drop %u irq %u\n", ifp->if_xname, lo, li, le, lt, ifp->if_iqdrops, irqs);
            } if (ifp->if_timer && --ifp->if_timer == 0 && ifp->if_watchdog) ifp->if_watchdog(ifp); }
        kdrv_netctl_t* c = ifp->if_ctl;                                          /* кадры от ядра -> очередь передачи карты */
        while (ifp->if_snd.ifq_len < ifp->if_snd.ifq_maxlen) {
            u32 head = __atomic_load_n(&c->tx_head, __ATOMIC_ACQUIRE), tail = c->tx_tail;
            if (head == tail) break;
            kdrv_slot_t* s = (kdrv_slot_t*)((u8*)c + KDRV_NET_TX_OFF) + (tail % c->slots);
            u32 len = s->len;
            if (len >= ETHER_HDR_LEN && len <= c->max_frame) {
                struct mbuf* m = m_gethdr(M_DONTWAIT, MT_DATA);
                if (m) { m_clget(m, M_DONTWAIT, MCLBYTES); }
                if (m && (m->m_flags & M_EXT)) { memcpy(m->m_data, s->data, len); m->m_len = m->m_pkthdr.len = (int)len; ifq_enqueue(&ifp->if_snd, m); ifp->if_opackets++; }
                else { if (m) m_freem(m); ifp->if_oerrors++; }
            } else ifp->if_oerrors++;
            __atomic_store_n(&c->tx_tail, tail + 1, __ATOMIC_RELEASE);
        }
        if (ifp->if_snd.ifq_len && ifp->if_start) ifp->if_start(ifp);
        ev = kdrv_irq_wait(use_irq ? 10 : 1);
        if (ev == KE_INTR) return 0;
        if (ev < 0) ev = 0;
    }
}
