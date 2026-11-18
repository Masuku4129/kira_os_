// Окно «Разум» с карточным кэшем: результат обязан побайтно совпадать с полной перерисовкой.
// Проверка дифференциальная: берём снимок, рисуем, меняем ровно один байт снимка (так по очереди каждый байт mind_view_t),
// рисуем с кэшем поверх прежнего рисунка и сравниваем с рисунком с нуля. Если подпись карточки забыла какое-то значение,
// которое карточка показывает, тест укажет страницу и смещение байта.
// Ещё: рисование с бюджетом времени по кадрам даёт тот же результат, неизменный снимок ничего не рисует.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "apps.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; if (fails < 20) printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define W 600
#define H 620
static uint32_t pxA[W * H], pxB[W * H];
static surface_t SA = { pxA, W, H, W }, SB = { pxB, W, H, W };

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }

static void fix_strings(mind_view_t* v) {
    v->mood[31] = 0; v->dev_name[15] = 0; v->hid_name[15] = 0; v->denied_name[15] = 0;
    for (int i = 0; i < MIND_VIEW_J; i++) v->j[i].text[MIND_TEXT - 1] = 0;
    for (int i = 0; i < 16; i++) v->p[i].name[15] = 0;
}
static void make_base(mind_view_t* v, int variant) {
    memset(v, 0, sizeof *v);
    v->t = 5000; v->lifetime_s = 90000 + (unsigned)variant; v->health = 77; v->c_resp = 90; v->c_room = 80; v->c_bal = 60; v->c_stab = 99;
    strcpy(v->mood, "сосредоточена"); v->mode = 1; v->quantum_ms = 30; v->cores = 4; v->mem_used = 33; v->runnable = 2;
    v->busy[0] = 70; v->busy[1] = 30; v->busy[2] = 20; v->busy[3] = 90;
    v->frames_per_s = 60; v->late_per_s = 2;
    for (int k = 0; k < MIND_SIG_N; k++) {
        v->cur_x100[k] = 1000 * (k + 1); v->mean_x100[k] = 900 * (k + 1); v->z_x10[k] = (k * 11) % 60 - 20; v->learned[k] = k == 3 ? 30 : 9000;
        for (int i = 0; i < MIND_HIST; i++) v->hist[k][i] = (uint8_t)((i * 3 + k * 17 + variant) % 101);
    }
    for (int i = 0; i < MIND_HIST; i++) v->health_hist[i] = (uint8_t)(60 + i % 30);
    v->hpos = 5; v->decisions = 40; v->anomalies = 3; v->kills = 1; v->tune_trials = 5; v->tune_wins = 2; v->activity = 1;
    v->nj = 4;
    for (unsigned i = 0; i < v->nj; i++) {
        v->j[i].t = 4000 + i * 10; v->j[i].kind = (uint8_t)(i % 5); v->j[i].applied = (uint8_t)(i & 1);
        snprintf(v->j[i].text, MIND_TEXT, "запись журнала номер %u", i);
    }
    v->nproc = 5;
    for (unsigned i = 0; i < v->nproc; i++) {
        v->p[i].pid = 10 + i; v->p[i].threads = 1 + i; v->p[i].heap_kb = 64 * (i + 1); v->p[i].cpu_x10 = 10 * i; v->p[i].sysc = 100 * i;
        v->p[i].weight = 100; v->p[i].limit = i == 2 ? 50 : 0; v->p[i].flagged = i == 1; v->p[i].focus = i == 0;
        snprintf(v->p[i].name, sizeof v->p[i].name, "prog%u", i);
    }
    v->risk = 40; v->risk_why = 2; v->net_n = 1000; v->net_pos = 300; v->net_prec = 70; v->net_rec = 60; v->forecasts = 80;
    v->net_loss_x1e4 = 2000; v->net_trusted = 1;
    for (int i = 0; i < MNET_F; i++) v->net_w[i] = (i % 3 - 1) * 10000 + i * 100;
    v->core_cap = 50; v->comp_par = 1; v->comp_ns_kpx_par = 800; v->comp_ns_kpx_ser = 1500;
    v->dev_errors = 1; strcpy(v->dev_name, "nvme0"); v->hid_suspect = 1; strcpy(v->hid_name, "mouse"); v->hid_rate = 900;
    v->denied = 2; strcpy(v->denied_name, "evil");
    for (int i = 0; i < ML_N; i++) { v->lv[i].val = i * 2 - 5; v->lv[i].trials = (uint16_t)(i + 1); v->lv[i].wins = (uint16_t)i; v->lv[i].backs = (uint16_t)(i % 3); v->lv[i].state = (uint8_t)(i % 4); }
    v->lv_changes = 9; v->lv_general = 1; v->lv_seed = 1;
    fix_strings(v);
}

static void full(surface_t* s, const mind_view_t* v, unsigned page) { app_mind_invalidate(); (void)app_mind_draw_step(s, v, page, 0, 0); }
static int same(void) { return memcmp(pxA, pxB, sizeof pxA) == 0; }

static uint64_t fake_t;
static uint64_t fake_now(void) { return fake_t += 1000000; }       /* каждый вызов часов - 1 мс: бюджет 1 нс даёт по одной карточке за шаг */

int main(void) {
    static mind_view_t base, v2;
    const unsigned sz = sizeof base;
    int checked = 0, skipped = 0;
    for (int variant = 0; variant < 2; variant++) {
        make_base(&base, variant);
        for (unsigned page = 0; page < 6; page++) {
            for (unsigned off = 0; off < sz; off++) {
                if (variant && off % 5) continue;                /* второй вариант - другие значения и другой бит, но не каждый байт: хватает времени */
                v2 = base;
                ((uint8_t*)&v2)[off] ^= (uint8_t)(variant ? 0x20 : 0x04);
                fix_strings(&v2);
                if (!memcmp(&v2, &base, sz)) { skipped++; continue; }
                /* Значения, которые приложение не должно получать: сам снимок их не производит (индексы и размеры в диапазоне). */
                if (v2.nj > MIND_VIEW_J || v2.nproc > 16 || v2.cores > 64 || v2.risk_why >= MNET_F || v2.risk_why < -1) { skipped++; continue; }
                memset(pxA, 0, sizeof pxA); memset(pxB, 0, sizeof pxB);
                full(&SA, &base, page);
                unsigned r = app_mind_draw_step(&SA, &v2, page, 0, 0);
                full(&SB, &v2, page);
                if (!same()) {
                    printf("FAIL: страница %u, байт %u (вариант %d): кэш расходится с полной перерисовкой\n", page, off, variant);
                    if (++fails > 20) return 1;
                }
                (void)r; checked++;
            }
        }
    }
    printf("каждый байт снимка: проверено %d, пропущено %d\n", checked, skipped);

    /* Неизменный снимок и неизменная поверхность: рисовать нечего. */
    make_base(&base, 0);
    memset(pxA, 0, sizeof pxA);
    full(&SA, &base, 2);
    CHECK(app_mind_draw_step(&SA, &base, 2, 0, 0) == 0);
    CHECK(app_mind_draw_step(&SA, &base, 2, 1, fake_now) == 0);
    /* Смена раздела - полный рисунок. */
    CHECK((app_mind_draw_step(&SA, &base, 3, 0, 0) & APP_MIND_DREW) != 0);
    CHECK(app_mind_draw_step(&SA, &base, 3, 0, 0) == 0);
    /* Сброс кэша - полный рисунок. */
    app_mind_invalidate();
    CHECK((app_mind_draw_step(&SA, &base, 3, 0, 0) & APP_MIND_DREW) != 0);

    /* Бюджет по кадрам: сколько бы шагов ни понадобилось, итог равен полной перерисовке. */
    int multi = 0;
    for (int it = 0; it < 120; it++) {
        unsigned page = (unsigned)it % 6;
        make_base(&base, it & 1);
        v2 = base;
        for (int k = 0; k < 6; k++) ((uint8_t*)&v2)[rnd() % sz] ^= (uint8_t)(1u << (rnd() % 6));
        fix_strings(&v2);
        if (v2.nj > MIND_VIEW_J || v2.nproc > 16 || v2.cores > 64 || v2.risk_why >= MNET_F || v2.risk_why < -1) continue;
        memset(pxA, 0, sizeof pxA); memset(pxB, 0, sizeof pxB);
        full(&SA, &base, page);
        int steps = 0;
        unsigned r;
        do { r = app_mind_draw_step(&SA, &v2, page, 1, fake_now); steps++; } while ((r & APP_MIND_MORE) && steps < 50);
        CHECK(steps < 50);
        if (steps > 2) multi++;
        CHECK(app_mind_draw_step(&SA, &v2, page, 1, fake_now) == 0);
        full(&SB, &v2, page);
        CHECK(same());
    }
    CHECK(multi > 0);                                    /* бюджет действительно делил работу на несколько кадров */

    if (fails) { printf("mind_cards: %d FAIL\n", fails); return 1; }
    printf("mind_cards: all PASS\n");
    return 0;
}
