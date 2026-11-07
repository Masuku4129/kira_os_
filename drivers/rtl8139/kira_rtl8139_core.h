/* KIRA RTL8139 datapath primitives.
 * Register-independent receive/transmit ring logic implemented from the
 * RTL8139 programming specification. No BSD-derived source is used here.
 */
#ifndef KIRA_RTL8139_CORE_H
#define KIRA_RTL8139_CORE_H
#include <stdint.h>
#include <stddef.h>

#define KIRA_RL_RX_RING_BYTES 65536u
#define KIRA_RL_RX_WRAP_BYTES 16u
#define KIRA_RL_RX_MIN_FRAME  4u
#define KIRA_RL_RX_MAX_FRAME  2048u
#define KIRA_RL_TX_SLOTS      4u
#define KIRA_RL_TX_MAX_FRAME  1792u
#define KIRA_RL_TX_MIN_FRAME  60u

typedef struct {
    uint32_t offset;
    uint32_t accepted, dropped, malformed;
} kira_rl_rx_state_t;

typedef struct {
    uint8_t busy[KIRA_RL_TX_SLOTS];
    uint8_t producer, consumer, used;
    uint32_t completed, errors;
} kira_rl_tx_state_t;

typedef struct {
    uint16_t status;
    uint16_t wire_length; /* includes the four-byte Ethernet FCS */
    uint16_t payload_length;
} kira_rl_rx_record_t;

/* RTL8139 RBSTART/TSAD registers are 32-bit DMA addresses. */
static inline int kira_rl_dma32_valid(uint64_t physical, uint64_t length) {
    return length != 0 && physical <= 0xffffffffull &&
           length - 1u <= 0xffffffffull - physical;
}

/* Copy a byte range from a power-of-two circular RX buffer. */
static inline int kira_rl_ring_copy_out(const uint8_t *ring, uint32_t ring_size,
                                        uint32_t offset, void *destination,
                                        uint32_t length) {
    if (!ring || !destination || !ring_size ||
        (ring_size & (ring_size - 1u)) || offset >= ring_size ||
        length > ring_size) return -1;
    uint32_t first = ring_size - offset;
    if (first > length) first = length;
    uint8_t *dst = (uint8_t *)destination;
    for (uint32_t i = 0; i < first; i++) dst[i] = ring[offset + i];
    for (uint32_t i = first; i < length; i++) dst[i] = ring[i - first];
    return 0;
}

static inline uint32_t kira_rl_ring_advance(uint32_t offset, uint32_t amount) {
    return (offset + amount) & (KIRA_RL_RX_RING_BYTES - 1u);
}

/* Decode the little-endian RTL8139 RX header and reject impossible lengths.
 * The caller must ensure the four-byte header itself is readable. */
static inline int kira_rl_rx_decode(const uint8_t h[4], kira_rl_rx_record_t *r) {
    if (!h || !r) return -1;
    r->status = (uint16_t)(h[0] | ((uint16_t)h[1] << 8));
    r->wire_length = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
    if (r->wire_length < KIRA_RL_RX_MIN_FRAME ||
        r->wire_length > KIRA_RL_RX_MAX_FRAME) return -1;
    r->payload_length = (uint16_t)(r->wire_length - 4u);
    return 0;
}

/* Status bits: ROK=bit 0, FAE=bit 1, CRC=bit 2, LONG=bit 3,
 * RUNT=bit 4, ISE=bit 5. Only a good packet is delivered. */
static inline int kira_rl_rx_status_ok(uint16_t status) {
    const uint16_t ROK = 1u << 0;
    const uint16_t BAD = (1u << 1) | (1u << 2) | (1u << 3) |
                         (1u << 4) | (1u << 5);
    return (status & ROK) != 0 && (status & BAD) == 0;
}

static inline int kira_rl_rx_account(kira_rl_rx_state_t *s,
                                     const kira_rl_rx_record_t *r,
                                     uint32_t *next_offset) {
    if (!s || !r || !next_offset) return -1;
    if (r->wire_length < KIRA_RL_RX_MIN_FRAME ||
        r->wire_length > KIRA_RL_RX_MAX_FRAME) {
        s->malformed++;
        return -1;
    }
    /* Hardware stores a four-byte header, frame incl. FCS, then aligns
       the next record to a four-byte boundary. */
    uint32_t span = (4u + (uint32_t)r->wire_length + 3u) & ~3u;
    s->offset = kira_rl_ring_advance(s->offset, span);
    *next_offset = s->offset;
    if (!kira_rl_rx_status_ok(r->status)) {
        s->dropped++;
        return 0;
    }
    s->accepted++;
    return 1;
}

static inline int kira_rl_tx_reserve(kira_rl_tx_state_t *s, uint8_t *slot) {
    if (!s || !slot || s->used >= KIRA_RL_TX_SLOTS) return -1;
    uint8_t n = s->producer;
    if (s->busy[n]) return -1;
    s->busy[n] = 1;
    s->producer = (uint8_t)((n + 1u) % KIRA_RL_TX_SLOTS);
    s->used++;
    *slot = n;
    return 0;
}
static inline int kira_rl_tx_complete(kira_rl_tx_state_t *s, uint8_t slot,
                                      int success) {
    if (!s || slot >= KIRA_RL_TX_SLOTS || !s->busy[slot]) return -1;
    s->busy[slot] = 0;
    if (s->used) s->used--;
    if (success) s->completed++; else s->errors++;
    s->consumer = (uint8_t)((slot + 1u) % KIRA_RL_TX_SLOTS);
    return 0;
}
static inline int kira_rl_tx_length(size_t requested, uint16_t *hardware_len) {
    if (!hardware_len || requested > KIRA_RL_TX_MAX_FRAME) return -1;
    if (requested < KIRA_RL_TX_MIN_FRAME) requested = KIRA_RL_TX_MIN_FRAME;
    *hardware_len = (uint16_t)requested;
    return 0;
}
#endif
