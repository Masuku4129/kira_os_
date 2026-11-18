/* Хост-тест блочного устройства драйвера Ring 3 (kdrv_blk.h): регистрация, границы запросов, кольца и учёт запросов в полёте, враждебные входы. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "kdrv_blk.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("kdrv_blk_test: FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static kdrv_blkreg_t reg(const char* name, uint64_t sectors) {
    kdrv_blkreg_t r; memset(&r, 0, sizeof r);
    memcpy(r.name, name, strlen(name) > sizeof r.name ? sizeof r.name : strlen(name));   /* 12 знаков заполняют поле без нуля */
    r.sectors = sectors; r.sector_size = 512;
    return r;
}
static kdrv_blkreq_t req(uint32_t op, uint64_t lba, uint32_t count, uint32_t slot) {
    kdrv_blkreq_t q; memset(&q, 0, sizeof q);
    q.id = 1; q.op = op; q.lba = lba; q.count = count; q.slot = slot;
    return q;
}

static uint32_t rng_state = 0x12345678u;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8 ^ rng_state; }

int main(void) {
    /* Раскладка общей памяти */
    CHECK(sizeof(kdrv_blkctl_t) <= KDRV_BLK_DATA_OFF);
    CHECK(KDRV_BLK_BYTES % 4096 == 0 && KDRV_BLK_SLOT_BYTES == 65536 && KDRV_BLK_BYTES == 4096 + 8 * 65536);
    CHECK(offsetof(kdrv_blkctl_t, req) % 8 == 0 && offsetof(kdrv_blkctl_t, rsp) % 4 == 0);

    /* Регистрация */
    kdrv_blkreg_t r = reg("vdb", 32768);
    CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);
    r = reg("sd0", 16); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);                            /* наименьший диск */
    r = reg("abcdefghijk", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);                  /* 11 знаков - предел */
    r = reg("abcdefghijkl", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);              /* 12 знаков */
    r = reg("", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);
    r = reg("VDB", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);                       /* заглавные */
    r = reg("vd-b", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);                      /* дефис не годится: имя идёт в команды disk/format */
    r = reg("vd b", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);
    r = reg("../x", 100); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);
    memset(&r, 'a', sizeof r.name); r.sectors = 100; r.sector_size = 512; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_NAME);   /* без нуля */
    r = reg("vdb", 100); r.sector_size = 4096; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SIZE);
    r.sector_size = 0; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SIZE);
    r = reg("vdb", 0); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SECTORS);
    r = reg("vdb", 15); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SECTORS);
    r = reg("vdb", KDRV_BLK_MAX_SECTORS); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);
    r = reg("vdb", KDRV_BLK_MAX_SECTORS + 1); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SECTORS);
    r = reg("vdb", ~0ull); CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_SECTORS);
    r = reg("vdb", 100); r.flags = KDRV_BLK_F_RO | KDRV_BLK_F_FLUSH; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);
    r.flags = 4; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_FLAGS);
    r.flags = 0; r.dma_bits = 32; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_OK);
    r.dma_bits = 19; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_BITS);
    r.dma_bits = 64; CHECK(kdrv_blk_reg_check(&r) == KDRV_BLK_E_BITS);

    /* Границы запросов: ёмкость 1000 секторов */
    const uint64_t cap = 1000;
    kdrv_blkreq_t q;
    q = req(KDRV_BLK_OP_READ, 0, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_OK);
    q = req(KDRV_BLK_OP_READ, 999, 1, 7); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_OK);             /* последний сектор */
    q = req(KDRV_BLK_OP_READ, 872, 128, 3); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_OK);          /* ровно до конца, полный буфер */
    q = req(KDRV_BLK_OP_READ, 873, 128, 3); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);     /* на сектор дальше */
    q = req(KDRV_BLK_OP_READ, 1000, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);      /* первый сектор за диском */
    q = req(KDRV_BLK_OP_WRITE, 999, 2, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);
    q = req(KDRV_BLK_OP_READ, ~0ull, 2, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);     /* lba + count переполняет 2^64 */
    q = req(KDRV_BLK_OP_READ, ~0ull - 1, 2, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);
    q = req(KDRV_BLK_OP_READ, 1ull << 63, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);
    q = req(KDRV_BLK_OP_READ, 0, 0, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);         /* пустая передача */
    q = req(KDRV_BLK_OP_READ, 0, 129, 0); CHECK(kdrv_blk_req_check(&q, 1u << 20, 0) == KDRV_BLK_E_RANGE);  /* больше буфера данных */
    q = req(KDRV_BLK_OP_READ, 0, 0xFFFFFFFFu, 0); CHECK(kdrv_blk_req_check(&q, ~0ull, 0) == KDRV_BLK_E_RANGE);
    q = req(KDRV_BLK_OP_READ, 0, 1, 8); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_SLOT);          /* буфера 8 нет */
    q = req(KDRV_BLK_OP_READ, 0, 1, 0xFFFFFFFFu); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_SLOT);
    q = req(0, 0, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_OP);
    q = req(4, 0, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_OP);
    q = req(0xFFFFFFFFu, 0, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_OP);
    q = req(KDRV_BLK_OP_WRITE, 5, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, KDRV_BLK_F_RO) == KDRV_BLK_E_RO);   /* запись на диск только для чтения */
    q = req(KDRV_BLK_OP_READ, 5, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, KDRV_BLK_F_RO) == KDRV_BLK_OK);
    q = req(KDRV_BLK_OP_FLUSH, 0, 0, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_OK);
    q = req(KDRV_BLK_OP_FLUSH, 0, 0, 0); CHECK(kdrv_blk_req_check(&q, cap, KDRV_BLK_F_RO) == KDRV_BLK_OK);  /* сброс кэша RO-диску не мешает */
    q = req(KDRV_BLK_OP_FLUSH, 5, 0, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);        /* у сброса нет диапазона */
    q = req(KDRV_BLK_OP_FLUSH, 0, 1, 0); CHECK(kdrv_blk_req_check(&q, cap, 0) == KDRV_BLK_E_RANGE);

    /* Кольцо: счётчики по модулю 2^32 */
    CHECK(kdrv_blk_ring_avail(0, 0, 8) == 0);
    CHECK(kdrv_blk_ring_avail(8, 0, 8) == 8);
    CHECK(kdrv_blk_ring_avail(9, 0, 8) == -1);                      /* head обогнал tail больше чем на кольцо */
    CHECK(kdrv_blk_ring_avail(2, 0xFFFFFFFEu, 8) == 4);             /* переход через 2^32 */
    CHECK(kdrv_blk_ring_avail(0xFFFFFFFFu, 0, 8) == -1);            /* head «позади» tail - это огромное число, не отрицательное */
    CHECK(kdrv_blk_ring_avail(0, 1, 8) == -1);
    CHECK(kdrv_blk_ring_avail(0x10000u, 0, 8) == -1);
    for (int i = 0; i < 200000; i++) {                              /* что бы ни лежало в head, результат лишь 0..slots или -1 */
        uint32_t h = rnd(), t = rnd();
        int a = kdrv_blk_ring_avail(h, t, 8);
        CHECK(a == -1 ? (uint32_t)(h - t) > 8 : (a >= 0 && a <= 8 && (uint32_t)a == h - t));
    }

    /* Учёт запросов в полёте */
    kdrv_blk_track_t t; kdrv_blk_track_init(&t);
    uint32_t id[9]; int sl[9];
    for (int i = 0; i < 8; i++) { sl[i] = kdrv_blk_track_alloc(&t, &id[i]); CHECK(sl[i] == i && id[i] == (uint32_t)i + 1); }
    CHECK(t.inflight == 8);
    CHECK(kdrv_blk_track_alloc(&t, &id[8]) == -1);                  /* все буферы заняты */
    CHECK(kdrv_blk_track_complete(&t, 999) == -1);                  /* номера не было */
    CHECK(kdrv_blk_track_complete(&t, 0) == -1);                    /* 0 - не номер */
    CHECK(kdrv_blk_track_complete(&t, id[3]) == 3 && t.inflight == 7);
    CHECK(kdrv_blk_track_complete(&t, id[3]) == -1 && t.inflight == 7);   /* повторный ответ на тот же запрос отвергается */
    CHECK(kdrv_blk_track_alloc(&t, &id[8]) == 3 && id[8] == 9);     /* освободившийся буфер получает новый номер */
    CHECK(kdrv_blk_track_complete(&t, id[3]) == -1);                /* старый номер больше не действует, пока буфер занят новым */
    t.next_id = 0xFFFFFFFFu; kdrv_blk_track_init(&t); t.next_id = 0xFFFFFFFFu;
    uint32_t a1, a2;
    CHECK(kdrv_blk_track_alloc(&t, &a1) == 0 && a1 == 0xFFFFFFFFu);
    CHECK(kdrv_blk_track_alloc(&t, &a2) == 1 && a2 == 1);           /* номер 0 пропущен при переполнении счётчика */
    kdrv_blk_track_init(&t);

    /* Враждебный драйвер: поток случайных ответов - принимаются лишь номера, реально находящиеся в полёте, и каждый один раз */
    uint32_t live[8]; int nlive = 0, accepted = 0, rejected = 0;
    for (int i = 0; i < 300000; i++) {
        if (rnd() % 3 == 0 && nlive < 8) { uint32_t n; int s = kdrv_blk_track_alloc(&t, &n); CHECK(s >= 0); live[nlive++] = n; }
        uint32_t guess = rnd() % 4 == 0 && nlive ? live[rnd() % nlive] : rnd();
        int s = kdrv_blk_track_complete(&t, guess);
        int was = 0, at = -1;
        for (int k = 0; k < nlive; k++) if (live[k] == guess) { was = 1; at = k; }
        CHECK((s >= 0) == was);
        if (was) { live[at] = live[--nlive]; accepted++; } else rejected++;
        CHECK((uint32_t)nlive == t.inflight);
    }
    CHECK(accepted > 1000 && rejected > 1000);

    if (fails) { printf("kdrv_blk_test: %d провал(ов)\n", fails); return 1; }
    printf("kdrv_blk_test: PASS (регистрация, границы, кольца, учёт запросов, %d принято и %d отвергнуто случайных ответов)\n", accepted, rejected);
    return 0;
}
