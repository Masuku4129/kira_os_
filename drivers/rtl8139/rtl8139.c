/* KIRA native RTL8139 driver.
 * Register definitions and state transitions are implemented independently
 * from the public RTL8139 programming specification. This file does not use
 * BSD bus_space/ifnet shims or BSD-derived driver source.
 */
#include <stdint.h>
#include <stddef.h>
#include "kira.h"
#include "kdrv.h"
#include "kdrv_format.h"
#include "kira_rtl8139_core.h"

#define RTL_VENDOR 0x10ECu
#define RTL_DEVICE 0x8139u
#define RTL_REG_BYTES 0x100u

enum {
    REG_IDR0=0x00, REG_MAR0=0x08,
    REG_TSD0=0x10, REG_TSAD0=0x20, REG_RBSTART=0x30,
    REG_CR=0x37, REG_CAPR=0x38, REG_CBR=0x3A,
    REG_IMR=0x3C, REG_ISR=0x3E, REG_TCR=0x40, REG_RCR=0x44,
    REG_MPC=0x4C, REG_CFG9346=0x50, REG_MSR=0x58,
    REG_BMCR=0x62, REG_BMSR=0x64
};
enum {
    CR_BUFE=0x01, CR_TE=0x04, CR_RE=0x08, CR_RST=0x10,
    ISR_RX_OK=0x0001, ISR_RX_ERR=0x0002,
    ISR_TX_OK=0x0004, ISR_TX_ERR=0x0008,
    TSD_TUN=0x00004000u, TSD_TOK=0x00008000u,
    TSD_TABT=0x40000000u,
    RCR_ACCEPT_UNICAST=0x02, RCR_ACCEPT_MULTICAST=0x04,
    RCR_ACCEPT_BROADCAST=0x08,
    RCR_FIFO_256=0x0000E000u, RCR_DMA_UNLIMITED=0x00000700u,
    RCR_BUFFER_64K=0x00001800u,
    TCR_IFG_96=0x03000000u, TCR_DMA_2048=0x00000700u,
    MSR_LINK_FAIL=0x04, BMSR_LINK=0x0004
};

#define RX_ALLOC_BYTES (KIRA_RL_RX_RING_BYTES + KIRA_RL_RX_WRAP_BYTES + 1536u)
#define TX_SLOT_BYTES 2048u
#define RX_BUDGET 64u
#define TX_EARLY_THRESHOLD 8u

typedef struct {
    volatile uint8_t *regs;
    kdrv_info_t info;
    kdrv_dma_t rx_dma, tx_dma;
    kdrv_netctl_t *net;
    uint8_t mac[6];
    kira_rl_rx_state_t rx;
    kira_rl_tx_state_t tx;
    uint64_t rx_bad, rx_dropped, tx_dropped;
    uint32_t link_ticks;
    int link_up;
} rtl8139_t;

static rtl8139_t dev;

static uint8_t r8(uint32_t off) {
    return *(volatile uint8_t*)(dev.regs+off);
}
static uint16_t r16(uint32_t off) {
    return *(volatile uint16_t*)(dev.regs+off);
}
static uint32_t r32(uint32_t off) {
    return *(volatile uint32_t*)(dev.regs+off);
}
static void w8(uint32_t off,uint8_t value) {
    *(volatile uint8_t*)(dev.regs+off)=value;
}
static void w16(uint32_t off,uint16_t value) {
    *(volatile uint16_t*)(dev.regs+off)=value;
}
static void w32(uint32_t off,uint32_t value) {
    *(volatile uint32_t*)(dev.regs+off)=value;
}
static void io_barrier(void) { __sync_synchronize(); }

static int wait_reset(void) {
    uint64_t deadline=k_clock_ns()+1000000000ull;
    while(r8(REG_CR)&CR_RST) {
        if(k_clock_ns()>=deadline) return -1;
        k_sleep(1);
    }
    return 0;
}
static int reset_chip(void) {
    w8(REG_CR,CR_RST);
    return wait_reset();
}

static int read_station_address(void) {
    int all_zero=1,all_ff=1;
    for(unsigned i=0;i<6;i++) {
        dev.mac[i]=r8(REG_IDR0+i);
        if(dev.mac[i]) all_zero=0;
        if(dev.mac[i]!=0xff) all_ff=0;
    }
    if(all_zero||all_ff||(dev.mac[0]&1u)) return -1;
    return 0;
}

static int configure_chip(void) {
    if(!kira_rl_dma32_valid(dev.rx_dma.phys,RX_ALLOC_BYTES)||
       !kira_rl_dma32_valid(dev.tx_dma.phys,KIRA_RL_TX_SLOTS*TX_SLOT_BYTES))
        return -1;

    dev.rx.offset=0;
    for(unsigned i=0;i<KIRA_RL_TX_SLOTS;i++) dev.tx.busy[i]=0;
    dev.tx.producer=dev.tx.consumer=0;
    dev.tx.used=dev.tx.completed=dev.tx.errors=0;
    w8(REG_CR,0);
    w16(REG_IMR,0);
    w16(REG_ISR,0xffff);
    w32(REG_RBSTART,(uint32_t)dev.rx_dma.phys);
    w16(REG_CAPR,0xfff0u);
    w32(REG_MPC,0);
    for(unsigned i=0;i<KIRA_RL_TX_SLOTS;i++) {
        w32(REG_TSAD0+4u*i,(uint32_t)(dev.tx_dma.phys+(uint64_t)i*TX_SLOT_BYTES));
        w32(REG_TSD0+4u*i,0);
    }
    /* Accept this station, broadcast and multicast; no promiscuous mode. */
    w32(REG_MAR0,0xffffffffu);
    w32(REG_MAR0+4,0xffffffffu);
    w32(REG_TCR,TCR_IFG_96|TCR_DMA_2048);
    w32(REG_RCR,RCR_FIFO_256|RCR_DMA_UNLIMITED|RCR_BUFFER_64K|
                  RCR_ACCEPT_UNICAST|RCR_ACCEPT_MULTICAST|RCR_ACCEPT_BROADCAST);
    w16(REG_ISR,0xffff);
    w16(REG_IMR,0);                 /* RTL8139 has no MSI; poll and use KDRV TX wakeups. */
    io_barrier();
    w8(REG_CR,CR_RE|CR_TE);
    return 0;
}

static int recover_device(void) {
    if(reset_chip()!=0) return -1;
    return configure_chip();
}

static int link_status(void) {
    (void)r16(REG_BMSR);
    uint16_t bmsr=(uint16_t)r16(REG_BMSR);    /* BMSR link bit is latch-low on this PHY. */
    return (bmsr&BMSR_LINK)!=0 || (r8(REG_MSR)&MSR_LINK_FAIL)==0;
}

static int deliver_frame(const uint8_t *frame,uint32_t length) {
    uint32_t head=__atomic_load_n(&dev.net->rx_head,__ATOMIC_RELAXED);
    uint32_t tail=__atomic_load_n(&dev.net->rx_tail,__ATOMIC_ACQUIRE);
    if(length<14u||length>dev.net->max_frame||head-tail>=dev.net->slots) {
        dev.rx_dropped++;
        return 0;
    }
    kdrv_slot_t *slot=(kdrv_slot_t*)((uint8_t*)dev.net+KDRV_NET_RX_OFF)+
                      (head%dev.net->slots);
    for(uint32_t i=0;i<length;i++) slot->data[i]=frame[i];
    slot->len=length;
    __atomic_store_n(&dev.net->rx_head,head+1u,__ATOMIC_RELEASE);
    return 1;
}

static int receive_poll(void) {
    uint8_t frame[KDRV_NET_MAXFRAME];
    unsigned budget=RX_BUDGET;
    int delivered=0;
    while(budget-- && !(r8(REG_CR)&CR_BUFE)) {
        uint32_t old=dev.rx.offset;
        volatile uint8_t *ring=(volatile uint8_t*)dev.rx_dma.va;
        if(((uint16_t)(ring[old+2u]|((uint16_t)ring[old+3u]<<8)))==0xfff0u)
            break;                  /* chip has not finished writing this record */
        uint8_t header[4]={ring[old],ring[old+1u],ring[old+2u],ring[old+3u]};
        kira_rl_rx_record_t record;
        if(kira_rl_rx_decode(header,&record)!=0||
           record.wire_length<64u||record.wire_length>1522u) {
            dev.rx_bad++;
            if(recover_device()!=0) return -1;
            return delivered;
        }
        uint32_t next=old;
        int accepted=kira_rl_rx_account(&dev.rx,&record,&next);
        if(accepted<0) {
            dev.rx_bad++;
            if(recover_device()!=0) return -1;
            return delivered;
        }
        if(accepted>0) {
            uint32_t payload=record.payload_length;
            uint32_t start=(old+4u)&(KIRA_RL_RX_RING_BYTES-1u);
            if(payload<=sizeof frame&&
               kira_rl_ring_copy_out((const uint8_t*)dev.rx_dma.va,
                    KIRA_RL_RX_RING_BYTES,start,frame,payload)==0) {
                delivered+=deliver_frame(frame,payload);
            } else dev.rx_dropped++;
        }
        io_barrier();
        w16(REG_CAPR,(uint16_t)(next-16u));
    }
    if(delivered) (void)kdrv_net_kick();
    return delivered;
}

static void reap_transmits(void) {
    while(dev.tx.used) {
        uint8_t slot=dev.tx.consumer;
        if(!dev.tx.busy[slot]) break;
        uint32_t status=r32(REG_TSD0+4u*slot);
        int done=kira_rl_tx_reap(&dev.tx,slot,status,TSD_TOK,TSD_TUN|TSD_TABT);
        if(done<=0) break;
    }
}

static void submit_transmits(void) {
    for(;;) {
        uint32_t head=__atomic_load_n(&dev.net->tx_head,__ATOMIC_ACQUIRE);
        uint32_t tail=__atomic_load_n(&dev.net->tx_tail,__ATOMIC_RELAXED);
        if(head==tail||dev.tx.used>=KIRA_RL_TX_SLOTS) break;
        kdrv_slot_t *slot=(kdrv_slot_t*)((uint8_t*)dev.net+KDRV_NET_TX_OFF)+
                          (tail%dev.net->slots);
        uint16_t hardware_length=0;
        uint32_t length=slot->len;
        if(length<14u||length>dev.net->max_frame||
           kira_rl_tx_length(length,&hardware_length)!=0) {
            __atomic_store_n(&dev.net->tx_tail,tail+1u,__ATOMIC_RELEASE);
            dev.tx_dropped++;
            continue;
        }
        uint8_t hw_slot=0;
        if(kira_rl_tx_reserve(&dev.tx,&hw_slot)!=0) break;
        uint8_t *buffer=(uint8_t*)dev.tx_dma.va+(uint32_t)hw_slot*TX_SLOT_BYTES;
        for(uint32_t i=0;i<length;i++) buffer[i]=slot->data[i];
        for(uint32_t i=length;i<hardware_length;i++) buffer[i]=0;
        io_barrier();
        w32(REG_TSAD0+4u*hw_slot,
            (uint32_t)(dev.tx_dma.phys+(uint64_t)hw_slot*TX_SLOT_BYTES));
        w32(REG_TSD0+4u*hw_slot,
            (TX_EARLY_THRESHOLD<<16)|(uint32_t)hardware_length);
        io_barrier();
        __atomic_store_n(&dev.net->tx_tail,tail+1u,__ATOMIC_RELEASE);
    }
}

static int bind_device(void) {
    if(kdrv_info(&dev.info)!=0||dev.info.vendor!=RTL_VENDOR||
       dev.info.device!=RTL_DEVICE) return -1;
    unsigned bar=6;
    for(unsigned i=0;i<6;i++)
        if(dev.info.bar[i].size>=RTL_REG_BYTES&&
           !(dev.info.bar[i].flags&KDRV_BAR_IO)) { bar=i; break; }
    if(bar==6) return -2;
    i64 error=0;
    dev.regs=(volatile uint8_t*)kdrv_mmio_map(bar,0,RTL_REG_BYTES,&error);
    if(!dev.regs) return -3;

    i64 command=kdrv_cfg_read(0x04,2);
    if(command<0||kdrv_cfg_write(0x04,2,(uint32_t)command|0x0006u|0x0400u)!=0)
        return -4;                  /* memory decode + bus master, disable INTx */
    if(kdrv_dma_alloc(RX_ALLOC_BYTES,32,&dev.rx_dma)!=0||
       kdrv_dma_alloc(KIRA_RL_TX_SLOTS*TX_SLOT_BYTES,32,&dev.tx_dma)!=0)
        return -5;
    if(!kira_rl_dma32_valid(dev.rx_dma.phys,RX_ALLOC_BYTES)||
       !kira_rl_dma32_valid(dev.tx_dma.phys,KIRA_RL_TX_SLOTS*TX_SLOT_BYTES))
        return -6;
    if(reset_chip()!=0||read_station_address()!=0||configure_chip()!=0)
        return -7;
    dev.net=kdrv_net_register(dev.mac);
    if(!dev.net||dev.net->slots!=KDRV_NET_SLOTS||
       dev.net->slot_size!=KDRV_NET_SLOT||dev.net->max_frame<1518u||
       dev.net->max_frame>KDRV_NET_MAXFRAME)
        return -8;
    dev.link_up=link_status();
    __atomic_store_n(&dev.net->link,dev.link_up?1u:0u,__ATOMIC_RELEASE);
    return 0;
}

static void stop_device(void) {
    if(dev.regs) {
        w16(REG_IMR,0);
        w8(REG_CR,0);
        io_barrier();
    }
}

int main(void) {
    if(bind_device()!=0) {
        k_puts("rtl8139: bind, MMIO, DMA or network registration failed\n");
        stop_device();
        return 1;
    }
    k_puts("rtl8139: native KIRA driver online; MAC ");
    for(unsigned i=0;i<6;i++) {
        static const char hex[]="0123456789abcdef";
        char pair[2]={hex[dev.mac[i]>>4],hex[dev.mac[i]&15]};
        k_write(pair,2);
        if(i!=5) k_puts(":");
    }
    k_puts("\n");

    for(;;) {
        i64 event=kdrv_irq_wait(1); /* bounded 10 ms poll; kernel TX events wake this wait early */
        if(event==KE_INTR) break;
        if(event<0&&event!=KE_TIMEOUT) k_sleep(1);
        reap_transmits();
        submit_transmits();
        if(receive_poll()<0) {
            k_puts("rtl8139: receive recovery failed; stopping\n");
            break;
        }
        if(++dev.link_ticks>=50u) {
            int up=link_status();
            dev.link_ticks=0;
            if(up!=dev.link_up) {
                dev.link_up=up;
                __atomic_store_n(&dev.net->link,up?1u:0u,__ATOMIC_RELEASE);
            }
        }
    }
    stop_device();
    return 0;
}
