/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef KIRA_BSD_CORE_H
#define KIRA_BSD_CORE_H
/* Слой совместимости с драйверным API OpenBSD для драйверов Ring 3 Киры (docs/BSD_COMPAT.md). Все заголовки <sys/...>, <net/...>, <machine/...>,
   <dev/pci/...> в kcc/include/bsd - тонкие обёртки над этим файлом; реализация - userland/bsd_rt.c. Драйвер однопоточный: его обработчики
   прерываний вызываются из главного цикла (опрос), поэтому spl*() ничего не маскирует. Прерываний INTx у Ring 3 нет - см. pci_intr_establish. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "kira.h"
#include "kdrv.h"

typedef uint8_t u_int8_t; typedef uint16_t u_int16_t; typedef uint32_t u_int32_t; typedef uint64_t u_int64_t;
typedef unsigned char u_char; typedef unsigned short u_short; typedef unsigned int u_int; typedef unsigned long u_long;
typedef char* caddr_t; typedef long ssize_t_bsd;
#ifndef NULL
#define NULL ((void*)0)
#endif
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define __packed __attribute__((packed))
#define __unused __attribute__((unused))
#define __predict_false(x) (x)
#define __predict_true(x) (x)
#define KASSERT(x) do { if (!(x)) panic("assertion failed: %s", #x); } while (0)
#define ALIGN(x) (((unsigned long)(x) + 7ul) & ~7ul)
#define ALIGNED_POINTER(p, t) ((((unsigned long)(p)) & (sizeof(t) - 1)) == 0)
#define swap16(x) ((u_int16_t)__builtin_bswap16(x))
#define swap32(x) ((u_int32_t)__builtin_bswap32(x))
static inline unsigned min(unsigned a, unsigned b) { return a < b ? a : b; }
static inline unsigned max(unsigned a, unsigned b) { return a > b ? a : b; }
#define M_DEVBUF 2
#define M_NOWAIT 1
#define M_WAITOK 2
#define M_ZERO 8

/* порядок байт: без этих определений `#if BYTE_ORDER == BIG_ENDIAN` истинно (оба имени = 0) и драйвер молча переставляет байты */
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN

/* ---- errno ---- */
#define EPERM 1
#define ENOENT 2
#define EIO 5
#define ENXIO 6
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#define ENOTTY 25
#define EAFNOSUPPORT 47
#define ENOBUFS 55
#define EOPNOTSUPP 45
#define ETIMEDOUT 60

/* ---- вывод и журнал ---- */
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_INFO 6
#define LOG_DEBUG 7
void printf(const char* fmt, ...);
void log(int level, const char* fmt, ...);
void panic(const char* fmt, ...) __attribute__((noreturn));
int snprintf(char* buf, size_t n, const char* fmt, ...);
#define DPRINTF(x) do { } while (0)

/* ---- память, строки, время ---- */
void* malloc(size_t n, int type, int flags);
void* mallocarray(size_t a, size_t b, int type, int flags);
void free(void* p, int type, size_t size);
void bcopy(const void* src, void* dst, size_t n);
void bzero(void* p, size_t n);
int bcmp(const void* a, const void* b, size_t n);
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
size_t strlen(const char* s);
size_t strlcpy(char* d, const char* s, size_t n);
void delay(unsigned us);
#define DELAY(x) delay(x)
#define hz 100
#define tick_ms 10
static inline int splnet(void) { return 0; }
static inline int splimp(void) { return 0; }
static inline int splbio(void) { return 0; }
static inline void splx(int s) { (void)s; }
#define IPL_NET 4
#define IPL_BIO 3
#define IPL_MPSAFE 0x100
#define ETHER_ADDR_LEN 6
#define ETHER_CRC_LEN 4
#define ETHER_HDR_LEN 14
#define ETHER_MIN_LEN 64
#define ETHER_MAX_LEN 1518
#define ETHER_VLAN_ENCAP_LEN 4
#define ETHERMTU 1500
#define IFNAMSIZ 16
#define htole16(x) ((u_int16_t)(x))
#define htole32(x) ((u_int32_t)(x))
#define letoh16(x) ((u_int16_t)(x))
#define letoh32(x) ((u_int32_t)(x))
#define le16toh(x) ((u_int16_t)(x))
#define le32toh(x) ((u_int32_t)(x))
#define htobe16(x) ((u_int16_t)(__builtin_bswap16(x)))
#define htobe32(x) ((u_int32_t)(__builtin_bswap32(x)))
#define betoh16(x) htobe16(x)
#define betoh32(x) htobe32(x)

/* ---- device, cfattach, cfdriver ---- */
struct device { char dv_xname[16]; int dv_unit; struct device* dv_parent; int dv_flags; };
struct cfdata { int cf_unit; int cf_flags; };
struct cfattach {
    size_t ca_devsize;
    int (*ca_match)(struct device*, void*, void*);
    void (*ca_attach)(struct device*, struct device*, void*);
    int (*ca_detach)(struct device*, int);
    int (*ca_activate)(struct device*, int);
};
struct cfdriver { void* cd_devs; char* cd_name; int cd_class; };
#define DV_IFNET 2
#define DVACT_QUIESCE 2
#define DVACT_SUSPEND 3
#define DVACT_RESUME 4
#define DVACT_WAKEUP 5
#define DETACH_FORCE 1
#define DETACH_QUIET 2
#define device_unref(d) ((void)0)

/* ---- mbuf ---- */
#define MSIZE 256
#define MCLBYTES 2048
#define MHLEN 192
#define MLEN 224
#define MINCLSIZE (MHLEN + MLEN)
#define M_DONTWAIT 1
#define MT_DATA 1
#define M_PKTHDR 0x0002
#define M_EXT 0x0001
#define M_BCAST 0x0100
#define M_MCAST 0x0200
struct pkthdr { int len; unsigned ph_ifidx; };
struct mbuf {
    struct mbuf* m_next; struct mbuf* m_nextpkt;
    caddr_t m_data; int m_len; short m_type; unsigned short m_flags;
    struct pkthdr m_pkthdr;
    char* m_ext; char m_buf[MLEN];
};
struct mbuf_list { struct mbuf* ml_head; struct mbuf* ml_tail; unsigned ml_len; };
#define MBUF_LIST_INITIALIZER() { NULL, NULL, 0 }
#define mtod(m, t) ((t)((m)->m_data))
#define MGETHDR(m, how, type) ((m) = m_gethdr((how), (type)))
#define MGET(m, how, type) ((m) = m_get((how), (type)))
#define MCLGET(m, how) m_clget((m), (how), MCLBYTES)
#define M_TRAILINGSPACE(m) ((((m)->m_flags & M_EXT) ? (m)->m_ext + MCLBYTES : (m)->m_buf + sizeof((m)->m_buf)) - ((m)->m_data + (m)->m_len))
struct mbuf* m_gethdr(int how, int type);
struct mbuf* m_get(int how, int type);
struct mbuf* m_clget(struct mbuf* m, int how, unsigned len);
struct mbuf* m_free(struct mbuf* m);
void m_freem(struct mbuf* m);
void ml_enqueue(struct mbuf_list* ml, struct mbuf* m);
struct mbuf* ml_dequeue(struct mbuf_list* ml);
#define ml_init(ml) do { (ml)->ml_head = (ml)->ml_tail = NULL; (ml)->ml_len = 0; } while (0)
#define ml_empty(ml) ((ml)->ml_len == 0)
#define ml_len(ml) ((ml)->ml_len)

/* ---- ifmedia / mii (минимум: список режимов, текущий, change/status) ---- */
#define IFM_ETHER 0x20
#define IFM_AUTO 0
#define IFM_MANUAL 1
#define IFM_NONE 2
#define IFM_10_T 3
#define IFM_10_2 4
#define IFM_10_5 5
#define IFM_100_TX 6
#define IFM_FDX 0x100000
#define IFM_HDX 0x200000
#define IFM_SUBTYPE(w) ((w) & 0x1f)
#define IFM_TYPE(w) ((w) & 0xe0)
#define IFM_INST_ANY ((unsigned)-1)
#define IFM_AVALID 1
#define IFM_ACTIVE 2
struct ifnet;
struct ifreq;
struct ifmedia_entry { int ifm_media; int ifm_data; void* ifm_aux; };
struct ifmediareq { char ifm_name[16]; int ifm_current, ifm_mask, ifm_status, ifm_active, ifm_count; };
#define IFM_MAXENT 16
struct ifmedia {
    int ifm_mask; int ifm_media; struct ifmedia_entry* ifm_cur; int ifm_n; struct ifmedia_entry ifm_ent[IFM_MAXENT];
    int (*ifm_change)(struct ifnet*); void (*ifm_status)(struct ifnet*, struct ifmediareq*);
};
struct mii_data { struct ifmedia mii_media; struct ifnet* mii_ifp; int mii_media_active, mii_media_status; };
void ifmedia_init(struct ifmedia*, int, int (*)(struct ifnet*), void (*)(struct ifnet*, struct ifmediareq*));
void ifmedia_add(struct ifmedia*, int, int, void*);
void ifmedia_set(struct ifmedia*, int);
int ifmedia_ioctl(struct ifnet*, struct ifreq*, struct ifmedia*, u_long);
void ifmedia_delete_instance(struct ifmedia*, unsigned);

/* ---- ifnet, arpcom, ether ---- */
#define IFF_UP 0x1
#define IFF_BROADCAST 0x2
#define IFF_DEBUG 0x4
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
#define IFF_PROMISC 0x100
#define IFF_ALLMULTI 0x200
#define IFF_OACTIVE 0x400
#define IFF_SIMPLEX 0x800
#define IFF_MULTICAST 0x8000
#define IFCAP_VLAN_MTU 0x10
#define SIOCSIFADDR 1
#define SIOCSIFFLAGS 2
#define SIOCGIFMEDIA 3
#define SIOCSIFMEDIA 4
#define SIOCADDMULTI 5
#define SIOCDELMULTI 6
#define ENETRESET (-2)
#define BPF_DIRECTION_OUT 2
#define BPF_DIRECTION_IN 1
struct ifqueue { struct mbuf* ifq_head; struct mbuf* ifq_tail; unsigned ifq_len; int ifq_oactive; unsigned ifq_maxlen; };
struct ifnet {
    void* if_softc; char if_xname[IFNAMSIZ]; int if_flags; int if_timer; int if_capabilities; unsigned if_hardmtu; u_long if_baudrate;
    unsigned if_ipackets, if_opackets, if_ierrors, if_oerrors, if_collisions, if_iqdrops;
    void (*if_start)(struct ifnet*); int (*if_ioctl)(struct ifnet*, u_long, caddr_t); void (*if_watchdog)(struct ifnet*);
    struct ifqueue if_snd; void* if_bpf; unsigned if_index;
    kdrv_netctl_t* if_ctl;                       /* кольца ядра (ether_ifattach) */
};
struct ether_header { u_int8_t ether_dhost[6]; u_int8_t ether_shost[6]; u_int16_t ether_type; };
struct ether_multi { u_int8_t enm_addrlo[6]; u_int8_t enm_addrhi[6]; struct ether_multi* enm_next; };
struct arpcom { struct ifnet ac_if; u_int8_t ac_enaddr[ETHER_ADDR_LEN]; int ac_multirangecnt; struct ether_multi* ac_multi; };
struct ether_multistep { struct ether_multi* e_enm; };
#define ETHER_FIRST_MULTI(step, ac, enm) do { (step).e_enm = (ac)->ac_multi; (enm) = (step).e_enm; } while (0)
#define ETHER_NEXT_MULTI(step, enm) do { if ((step).e_enm) (step).e_enm = (step).e_enm->enm_next; (enm) = (step).e_enm; } while (0)
struct ifreq { char ifr_name[IFNAMSIZ]; struct ifmediareq ifr_media; };
#define ifq_set_oactive(q) ((q)->ifq_oactive = 1)
#define ifq_clr_oactive(q) ((q)->ifq_oactive = 0)
#define ifq_is_oactive(q) ((q)->ifq_oactive)
struct mbuf* ifq_dequeue(struct ifqueue* q);
void if_attach(struct ifnet*);
void if_detach(struct ifnet*);
void if_input(struct ifnet*, struct mbuf_list*);
void ether_ifattach(struct ifnet*);
void ether_ifdetach(struct ifnet*);
int ether_ioctl(struct ifnet*, struct arpcom*, u_long, caddr_t);
char* ether_sprintf(u_int8_t*);
u_int32_t ether_crc32_be(const u_int8_t*, size_t);
static inline void bpf_mtap(void* b, struct mbuf* m, int dir) { (void)b; (void)m; (void)dir; }

/* ---- bus_space: порты I/O (kdrv_io_*) и отображённая память (kdrv_mmio_map) ---- */
struct kd_bst { int bar; int io; };
typedef struct kd_bst* bus_space_tag_t;
typedef unsigned long bus_space_handle_t;       /* порты: смещение в BAR; память: адрес отображения */
typedef unsigned long bus_size_t;
typedef unsigned long bus_addr_t;
u_int32_t kd_bs_read(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w);
void kd_bs_write(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, u_int32_t v);
void kd_bs_read_multi(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, void* buf, size_t bytes);
void kd_bs_write_multi(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, int w, const void* buf, size_t bytes);
#define bus_space_read_1(t, h, o) ((u_int8_t)kd_bs_read((t), (h), (o), 1))
#define bus_space_read_2(t, h, o) ((u_int16_t)kd_bs_read((t), (h), (o), 2))
#define bus_space_read_4(t, h, o) ((u_int32_t)kd_bs_read((t), (h), (o), 4))
#define bus_space_write_1(t, h, o, v) kd_bs_write((t), (h), (o), 1, (v))
#define bus_space_write_2(t, h, o, v) kd_bs_write((t), (h), (o), 2, (v))
#define bus_space_write_4(t, h, o, v) kd_bs_write((t), (h), (o), 4, (v))
#define bus_space_read_multi_1(t, h, o, p, n) kd_bs_read_multi((t), (h), (o), 1, (p), (n))
#define bus_space_read_multi_2(t, h, o, p, n) kd_bs_read_multi((t), (h), (o), 2, (p), (size_t)(n) * 2)
#define bus_space_write_multi_1(t, h, o, p, n) kd_bs_write_multi((t), (h), (o), 1, (p), (n))
#define bus_space_write_multi_2(t, h, o, p, n) kd_bs_write_multi((t), (h), (o), 2, (p), (size_t)(n) * 2)
#define bus_space_read_raw_multi_2(t, h, o, p, bytes) kd_bs_read_multi((t), (h), (o), 2, (p), (bytes))
#define bus_space_write_raw_multi_2(t, h, o, p, bytes) kd_bs_write_multi((t), (h), (o), 2, (p), (bytes))
void bus_space_read_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int8_t* p, bus_size_t n);
void bus_space_write_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, const u_int8_t* p, bus_size_t n);
void bus_space_set_region_1(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, u_int8_t v, bus_size_t n);
int bus_space_subregion(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, bus_size_t size, bus_space_handle_t* nh);
void bus_space_unmap(bus_space_tag_t t, bus_space_handle_t h, bus_size_t size);
#define BUS_SPACE_BARRIER_READ 1
#define BUS_SPACE_BARRIER_WRITE 2
#define bus_space_barrier(t, h, o, l, f) __sync_synchronize()

/* ---- PCI ---- */
typedef void* pci_chipset_tag_t;
typedef u_int32_t pcireg_t; typedef u_int32_t pcitag_t;
typedef u_int16_t pci_vendor_id_t; typedef u_int16_t pci_product_id_t;
typedef int pci_intr_handle_t;
#define PCI_VENDOR(id) ((id) & 0xffff)
#define PCI_PRODUCT(id) (((id) >> 16) & 0xffff)
#define PCI_CLASS(r) (((r) >> 24) & 0xff)
#define PCI_SUBCLASS(r) (((r) >> 16) & 0xff)
#define PCI_ID_REG 0x00
#define PCI_COMMAND_STATUS_REG 0x04
#define PCI_CLASS_REG 0x08
#define PCI_MAPREG_START 0x10
#define PCI_MAPREG_TYPE_IO 0x01
#define PCI_MAPREG_TYPE_MEM 0x00
#define PCI_MAPREG_MEM_TYPE_64BIT 0x04
#define PCI_COMMAND_IO_ENABLE 0x1
#define PCI_COMMAND_MEM_ENABLE 0x2
#define PCI_COMMAND_MASTER_ENABLE 0x4
struct pci_attach_args {
    pci_chipset_tag_t pa_pc; pcitag_t pa_tag; pcireg_t pa_id, pa_class;
    bus_space_tag_t pa_iot, pa_memt; int pa_intrline;
};
int pci_mapreg_map(struct pci_attach_args*, int reg, pcireg_t type, int flags, bus_space_tag_t* tp, bus_space_handle_t* hp, bus_addr_t* basep, bus_size_t* sizep, bus_size_t maxsize);
pcireg_t pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg);
void pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t v);
int pci_intr_map(struct pci_attach_args*, pci_intr_handle_t*);
const char* pci_intr_string(pci_chipset_tag_t, pci_intr_handle_t);
void* pci_intr_establish(pci_chipset_tag_t, pci_intr_handle_t, int ipl, int (*fn)(void*), void* arg, const char* name);
void pci_intr_disestablish(pci_chipset_tag_t, void*);

/* ---- timeout (вызывается из главного цикла драйвера) ---- */
struct timeout { void (*to_fn)(void*); void* to_arg; unsigned long to_due; int to_set; };
void timeout_set(struct timeout*, void (*)(void*), void*);
void timeout_add_msec(struct timeout*, int);
#define timeout_add_sec(t, s) timeout_add_msec((t), (s) * 1000)
int timeout_del(struct timeout*);
#define timeout_pending(t) ((t)->to_set)
#endif
