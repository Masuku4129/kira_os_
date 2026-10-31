// Хост-тест программ fpu и burn: настоящий код против подменённых системных вызовов.
// Настоящую потерю регистров при переключении потоков ловит tests/ctx_fpu_test.c (реальный context.s); здесь - логика самих программ.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include "stub/kira.h"
char stub_log[4096];
#define umain fpu_umain
#define say fpu_say
#define say_u fpu_say_u
#include "../userland/fpu.c"
#undef umain
#undef say
#undef say_u
#define umain burn_umain
#define say burn_say
#define say_u burn_say_u
#define _start burn_start
#include "../userland/burn.c"
#undef umain

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
    // два вычисления с одними параметрами совпадают, с разными - нет (контроль самого теста)
    digest_t a = work(3), b = work(3), c = work(4);
    CHECK(same(&a, &b) && !same(&a, &c));
    // эталон зависит от каждого из трёх видов арифметики: x87 (long double), double, SSE
    digest_t d = work(5), e = work(6);
    CHECK(d.w[3] != e.w[3] && d.w[2] != e.w[2] && d.w[0] != e.w[0]);

    stub_ticks_now = 0; stub_ticks_step = 1; stub_pid = 7; stub_log[0] = 0;
    CHECK(fpu_umain() == 0);
    CHECK(strstr(stub_log, "PASS: FP/vector state is private to every process"));
    CHECK(strstr(stub_log, "mismatches 0"));

    // burn: считает до срока, печатает число итераций (кратное 4096: время проверяется блоками)
    stub_ticks_now = 0; stub_ticks_step = 50; stub_log[0] = 0;
    CHECK(burn_umain() == 0);
    const char* p = strstr(stub_log, "iterations ");
    CHECK(p != 0);
    unsigned long long it = p ? strtoull(p + 11, 0, 10) : 0;
    CHECK(it >= 4096 && it % 4096 == 0);

    if (fails) { printf("fpu/burn: %d FAILED\n", fails); return 1; }
    printf("fpu/burn: reference stability, distinct parameters, verdict and iteration counting passed\n");
    return 0;
}
