/* Реестр хуков сна: порядок вызовов, откат при отказе, замена по имени, журнал, устройство, не пережившее пробуждение. */
#include "../devpm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static char trace[256]; static int tn;
static char logbuf[8][200]; static int logn;
static void lg(const char* s) { if (logn < 8) { strncpy(logbuf[logn], s, 199); logbuf[logn][199] = 0; logn++; } }
static void t(char c) { trace[tn++] = c; trace[tn] = 0; }
static int s_a(void* x) { (void)x; t('a'); return 0; }  static int r_a(void* x) { (void)x; t('A'); return 0; }
static int s_b(void* x) { (void)x; t('b'); return 0; }  static int r_b(void* x) { (void)x; t('B'); return 0; }
static int s_c(void* x) { (void)x; t('c'); return 0; }  static int r_c(void* x) { (void)x; t('C'); return 0; }
static int s_bad(void* x) { (void)x; t('x'); return -5; }
static int r_bad(void* x) { (void)x; t('X'); return -7; }
static int r_ctx(void* x) { *(int*)x += 1; return 0; }
static int s_ctx(void* x) { *(int*)x += 10; return 0; }

int main(void) {
    devpm_set_log(lg);
    /* порядок: сон - обратный, пробуждение - прямой */
    CHECK(devpm_register("a", s_a, r_a, 0) == 0 && devpm_register("b", s_b, r_b, 0) == 0 && devpm_register("c", s_c, r_c, 0) == 0);
    CHECK(devpm_count() == 3);
    CHECK(devpm_suspend_all() == 0 && strcmp(trace, "cba") == 0);
    devpm_info_t in; CHECK(devpm_info(0, &in) == 0 && in.state == 1 && strcmp(in.name, "a") == 0);
    CHECK(devpm_resume_all() == 0 && strcmp(trace, "cbaABC") == 0);
    CHECK(devpm_info(2, &in) == 0 && in.state == 0);
    CHECK(devpm_info(3, &in) != 0 && devpm_info(-1, &in) != 0);
    CHECK(devpm_resume_all() == 0 && strcmp(trace, "cbaABC") == 0);                     /* не усыплённые не будятся */

    /* отказ посередине: усыплённые до него возвращаются в работу (в порядке регистрации), отказавший и ещё не тронутые - нет */
    tn = 0; trace[0] = 0; logn = 0;
    CHECK(devpm_register("bad", s_bad, r_bad, 0) == 0);                                    /* порядок: a b c bad -> сон: bad c b a */
    CHECK(devpm_suspend_all() < 0 && strcmp(trace, "x") == 0);                            /* bad - последний зарегистрирован, значит первый ко сну */
    CHECK(strcmp(devpm_refusal(), "bad") == 0 && devpm_refusal_code() == -5);
    CHECK(logn == 1 && strstr(logbuf[0], "[KIRA SLEEP] bad:") == logbuf[0] && strstr(logbuf[0], "сон отменён") && strstr(logbuf[0], "-5"));
    CHECK(devpm_resume_all() == 0 && strcmp(trace, "x") == 0);                            /* никого не усыпили - никого не будим */

    /* отказ в середине списка: bad регистрируется раньше c */
    devpm_register("bad", 0, 0, 0);                                                         /* без хуков - отказ регистрации, прежняя запись цела */
    CHECK(devpm_count() == 4);
    tn = 0; trace[0] = 0; logn = 0;
    CHECK(devpm_register("bad", s_bad, r_bad, 0) == 0 && devpm_count() == 4);              /* замена по имени, не дубль */
    /* переставим: новый реестр не вводим, проверяем откат через хук между a/b: b отказывает */
    CHECK(devpm_register("b", s_bad, r_b, 0) == 0);
    /* порядок a b c bad; ко сну bad -> отказ сразу; снимем отказ у bad */
    CHECK(devpm_register("bad", s_b, r_b, 0) == 0);
    tn = 0; trace[0] = 0; logn = 0;
    CHECK(devpm_suspend_all() < 0);                                                         /* bad(b) c(c) затем b(отказ) */
    CHECK(strcmp(trace, "bcxCB") == 0 && strcmp(devpm_refusal(), "b") == 0);
    /* откат: c и bad вернулись, в порядке регистрации (c, bad) */
    devpm_info(2, &in); CHECK(in.state == 0);
    CHECK(strstr(logbuf[0], "b:") != 0);
    CHECK(devpm_resume_all() == 0);

    /* устройство не восстановилось: журнал, остальные восстанавливаются, число неудач в результате */
    tn = 0; trace[0] = 0; logn = 0;
    devpm_register("b", s_b, r_bad, 0);
    CHECK(devpm_suspend_all() == 0);
    int bad = devpm_resume_all();
    CHECK(bad == 1);
    CHECK(logn == 1 && strstr(logbuf[0], "не восстановилось") && strstr(logbuf[0], "-7"));
    devpm_info(1, &in); CHECK(in.state == 2 && in.last_rc == -7);
    CHECK(devpm_suspend_all() == 0);                                                        /* следующий сон: устройство снова пробуется */
    devpm_resume_all();

    /* контекст передаётся хуку; замена имени сбрасывает состояние */
    int v = 0;
    CHECK(devpm_register("ctx", s_ctx, r_ctx, &v) == 0);
    CHECK(devpm_suspend_all() == 0 && v == 10);
    CHECK(devpm_resume_all() >= 0 && v == 11);
    CHECK(devpm_register(0, s_a, r_a, 0) < 0 && devpm_register("", s_a, r_a, 0) < 0);

    /* реестр ограничен, а не переполняется */
    static char names[DEVPM_MAX + 4][16]; int ok = 0, fail = 0;
    for (int i = 0; i < DEVPM_MAX + 4; i++) { snprintf(names[i], 16, "n%d", i); if (devpm_register(names[i], s_a, r_a, 0) == 0) ok++; else fail++; }
    CHECK(devpm_count() == DEVPM_MAX && fail > 0 && ok + 6 >= DEVPM_MAX - 6);
    printf("devpm: все проверки реестра сна пройдены (порядок, откат, журнал, замена, предел)\n");
    return 0;
}
