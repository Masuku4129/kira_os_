// Хост-тест решения об изоляции сбоя и построчного буфера журнала.
#include <stdio.h>
#include <string.h>
#include "corefault.h"
#include "linebuf.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static corefault_input_t base(unsigned vec) {
    corefault_input_t i = { .vector = vec, .have_thread = 1, .other_cores = 3, .irq_move_ok = 1 };
    return i;
}

static char out[2048];
static unsigned emits;
static void emit(const char* s, unsigned n, void* ctx) { (void)ctx; size_t l = strlen(out); memcpy(out + l, s, n); out[l + n] = 0; emits++; }

int main(void) {
    CHECK(corefault_decide(0) == COREFAULT_PANIC);
    CHECK(strcmp(corefault_why_panic(0), "missing fault context") == 0);
    corefault_input_t i = base(14);
    CHECK(corefault_decide(&i) == COREFAULT_KILL_THREAD);       // обычный сбой в потоке: гибнет поток
    CHECK(corefault_why_panic(&i) == 0);
    i = base(13); CHECK(corefault_decide(&i) == COREFAULT_KILL_THREAD);
    i = base(14); i.other_cores = 0; CHECK(corefault_decide(&i) == COREFAULT_KILL_THREAD);   // и на единственном ядре, если поток не служебный

    i = base(14); i.locks_held = 1;   CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.irq_depth = 1;    CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.in_fault = 1;     CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.service = 1;      CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.mutexes_held = 1; CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.have_thread = 0;  CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(8);                      CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(2);                      CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(14); i.locks_held = 1;   CHECK(corefault_why_panic(&i) && strstr(corefault_why_panic(&i), "spinlock"));

    i = base(18); CHECK(corefault_decide(&i) == COREFAULT_FAIL_CORE);          // машинная проверка: отказ ядра
    i = base(18); i.other_cores = 0;   CHECK(corefault_decide(&i) == COREFAULT_PANIC);   // забрать работу некому
    i = base(18); i.irq_move_ok = 0;   CHECK(corefault_decide(&i) == COREFAULT_PANIC);   // прерывания устройств не переселить
    i = base(18); i.service = 1;       CHECK(corefault_decide(&i) == COREFAULT_PANIC);
    i = base(18); i.locks_held = 2;    CHECK(corefault_decide(&i) == COREFAULT_PANIC);

    // Построчный буфер: строки двух «ядер» не перемешиваются
    linebuf_t a = {0}, b = {0};
    out[0] = 0;
    const char* x = "alpha-"; const char* y = "BRAVO-";
    for (int k = 0; k < 6; k++) { lb_putc(&a, x[k], emit, 0); lb_putc(&b, y[k], emit, 0); }
    CHECK(emits == 0 && out[0] == 0);                            // пока строка не закончена, ничего не выведено
    lb_putc(&a, '\n', emit, 0);
    CHECK(strcmp(out, "alpha-\n") == 0);
    lb_putc(&b, '\n', emit, 0);
    CHECK(strcmp(out, "alpha-\nBRAVO-\n") == 0);
    // Хвост без перевода строки выходит по тикам
    out[0] = 0; emits = 0;
    lb_putc(&a, '>', emit, 0);
    lb_tick(&a, emit, 0);
    CHECK(emits == 0);
    lb_tick(&a, emit, 0);
    CHECK(emits == 1 && strcmp(out, ">") == 0);
    lb_tick(&a, emit, 0); CHECK(emits == 1);                     // пустой буфер ничего не выводит
    // Переполнение: строка длиннее буфера выходит кусками, ничего не теряется
    out[0] = 0; emits = 0;
    for (int k = 0; k < LB_SIZE * 2 + 5; k++) lb_putc(&a, 'z', emit, 0);
    lb_flush(&a, emit, 0);
    CHECK(strlen(out) == LB_SIZE * 2 + 5 && emits == 3);
    // Продолжение сбрасывает возраст хвоста
    out[0] = 0; emits = 0;
    lb_putc(&a, 'q', emit, 0); lb_tick(&a, emit, 0); lb_putc(&a, 'w', emit, 0); lb_tick(&a, emit, 0);
    CHECK(emits == 0);
    lb_tick(&a, emit, 0); CHECK(emits == 1 && strcmp(out, "qw") == 0);

    printf("%s\n", fails ? "FAILED" : "ALL COREFAULT/LINEBUF TESTS PASSED");
    return fails != 0;
}
