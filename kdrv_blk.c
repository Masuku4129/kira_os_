/* Чистые проверки блочного устройства драйвера Ring 3 (kdrv_blk.h): без обращений к ядру, их используют kdrv.c, эталонный драйвер
   userland/drvvblk.c и хост-тест tests/kdrv_blk_test.c. Входу не доверяют: каждое поле проверяется, переполнения исключены. */
#include "kdrv_blk.h"

int kdrv_blk_reg_check(const kdrv_blkreg_t* r) {
    unsigned i = 0;
    while (i < sizeof r->name && r->name[i]) {
        char c = r->name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return KDRV_BLK_E_NAME;
        i++;
    }
    if (i < 1 || i > KDRV_BLK_NAME_MAX || i >= sizeof r->name) return KDRV_BLK_E_NAME;       /* нуль внутри поля обязателен */
    if (r->sector_size != KDRV_BLK_SECTOR) return KDRV_BLK_E_SIZE;
    if (r->sectors < 16 || r->sectors > KDRV_BLK_MAX_SECTORS) return KDRV_BLK_E_SECTORS;
    if (r->flags & ~KDRV_BLK_F_KNOWN) return KDRV_BLK_E_FLAGS;
    if (r->dma_bits && (r->dma_bits < 20 || r->dma_bits > 63)) return KDRV_BLK_E_BITS;
    return KDRV_BLK_OK;
}

int kdrv_blk_req_check(const kdrv_blkreq_t* q, uint64_t sectors, uint32_t flags) {
    if (q->op != KDRV_BLK_OP_READ && q->op != KDRV_BLK_OP_WRITE && q->op != KDRV_BLK_OP_FLUSH) return KDRV_BLK_E_OP;
    if (q->op == KDRV_BLK_OP_FLUSH) return q->count == 0 && q->lba == 0 ? KDRV_BLK_OK : KDRV_BLK_E_RANGE;
    if (q->op == KDRV_BLK_OP_WRITE && (flags & KDRV_BLK_F_RO)) return KDRV_BLK_E_RO;
    if (q->slot >= KDRV_BLK_SLOTS) return KDRV_BLK_E_SLOT;
    if (q->count == 0 || q->count > KDRV_BLK_SLOT_SECTORS) return KDRV_BLK_E_RANGE;
    if (q->lba >= sectors || (uint64_t)q->count > sectors - q->lba) return KDRV_BLK_E_RANGE;     /* без сложения: переполнение невозможно */
    return KDRV_BLK_OK;
}

int kdrv_blk_ring_avail(uint32_t head, uint32_t tail, uint32_t slots) {
    uint32_t n = head - tail;                       /* по модулю 2^32 */
    return n > slots ? -1 : (int)n;
}

void kdrv_blk_track_init(kdrv_blk_track_t* t) {
    for (unsigned i = 0; i < KDRV_BLK_SLOTS; i++) { t->id[i] = 0; t->state[i] = KDRV_BLK_T_FREE; }
    t->next_id = 1; t->inflight = 0;
}

int kdrv_blk_track_alloc(kdrv_blk_track_t* t, uint32_t* id) {
    for (unsigned i = 0; i < KDRV_BLK_SLOTS; i++) {
        if (t->state[i] != KDRV_BLK_T_FREE) continue;
        uint32_t n = t->next_id++;
        if (n == 0) n = t->next_id++;               /* 0 - не номер запроса: «пустая» ячейка */
        t->id[i] = n; t->state[i] = KDRV_BLK_T_POSTED; t->inflight++;
        *id = n;
        return (int)i;
    }
    return -1;
}

int kdrv_blk_track_complete(kdrv_blk_track_t* t, uint32_t id) {
    if (id == 0) return -1;
    for (unsigned i = 0; i < KDRV_BLK_SLOTS; i++)
        if (t->state[i] == KDRV_BLK_T_POSTED && t->id[i] == id) { t->state[i] = KDRV_BLK_T_FREE; t->inflight--; return (int)i; }
    return -1;
}
