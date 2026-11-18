/* x86-64: команды консоли kaslr, kptr и wx - проверка защиты образа ядра (случайный адрес базы, политика печати адресов,
   W^X). Их выводом пользуются сценарии QEMU kaslr и wx-early (ci/scenarios.py). */
#include "kaslr_arch.h"
#include "kaslr.h"
#include "kptr.h"
#include "../../uart.h"
#include "../../vmm.h"

extern char kernel_start[], kernel_end[], wx_text_start[], wx_text_end[], wx_ro_start[], wx_ro_end[];
extern int uaccess_copy(void* dst, const void* src, uint64_t n);     /* uaccess.s: #PF внутри копирования даёт -1, а не аварию */

static int word_is(const char* s, const char* w) { while (*w) if (*s++ != *w++) return 0; return *s == 0 || *s == ' '; }
static const char* skip_word(const char* s) { while (*s && *s != ' ') s++; while (*s == ' ') s++; return s; }

/* kaslr: состояние случайного адреса ядра. Настоящий адрес базы - только в режиме отладки (политика kptr). */
void kaslr_cmd(const char* a) {
    (void)a;
    const kaslr_state_t* s = kaslr_info();
    if (s->slide == 0) {
        uart_print("[KIRA KASLR] выключен: "); uart_print(kaslr_reason(s->reason)); uart_print("\n");
        return;
    }
    uart_print("[KIRA KASLR] включён: образ ядра перемещён в случайную 2-МБ позицию (1 из "); uart_print_dec(s->slots);
    uart_print("), исправлено слов: "); uart_print_dec(s->relocs); uart_print(", энтропия:");
    if (s->entropy_src & KASLR_SRC_RDSEED) uart_print(" rdseed");
    if (s->entropy_src & KASLR_SRC_RDRAND) uart_print(" rdrand");
    uart_print(" tsc+pit\n");
    if (kptr_debug()) {
        uart_print("[KIRA KASLR] kernel base "); uart_print_hex(s->base);
        uart_print(" (link base "); uart_print_hex(s->link_base); uart_print(", slide "); uart_print_hex(s->slide); uart_print(")\n");
    } else {
        uart_print("[KIRA KASLR] адрес базы скрыт политикой kptr (включить показ: kptr debug on)\n");
    }
}

/* kptr [debug on|off]: политика печати адресов ядра. */
void kptr_cmd(const char* a) {
    while (*a == ' ') a++;
    if (word_is(a, "debug")) {
        const char* v = skip_word(a);
        if (word_is(v, "on")) kptr_set_debug(1);
        else if (word_is(v, "off")) kptr_set_debug(0);
        else if (*v) { uart_print("использование: kptr [debug on|off]\n"); return; }
    } else if (*a) { uart_print("использование: kptr [debug on|off]\n"); return; }
    uart_print("[KIRA KPTR] режим отладки: "); uart_print(kptr_debug() ? "включён (адреса ядра печатаются как есть)" : "выключен (адреса образа ядра печатаются как K+смещение)");
    uart_print("; пример: "); kptr_print((uint64_t)(uintptr_t)&kptr_cmd); uart_print("\n");
}

/* Пробная запись одного байта (его же значения) по адресу kernel-страницы: на странице без права записи процессор даёт
   #PF, uaccess_copy превращает его в -1; значит защита работает. 0 - запись удалась (защиты нет). */
static int write_traps(const void* where) {
    uint8_t b = *(volatile const uint8_t*)where;
    return uaccess_copy((void*)where, &b, 1) != 0;
}

/* wx: W^X образа ядра. Итог ранней проверки (до vmm_enforce_wx), обход таблиц сейчас и пробные записи в .text и .rodata. */
void wx_cmd(const char* a) {
    (void)a;
    int early = vmm_early_wx_state();
    uart_print("[KIRA WX] ранняя карта (boot.s/earlymap.c, до vmm_enforce_wx): ");
    uart_print(early > 0 ? "без страниц W+X с первой инструкции\n" : early < 0 ? "БЫЛИ страницы W+X\n" : "не проверялась\n");
    int now = vmm_verify_wx(1);
    int t = write_traps(wx_text_start + 16), r = write_traps(wx_ro_start + 16);
    uart_print("[KIRA WX] запись в .text: "); uart_print(t ? "ловушка сработала (#PF), страница только для исполнения\n" : "ПРОШЛА - защиты нет\n");
    uart_print("[KIRA WX] запись в .rodata: "); uart_print(r ? "ловушка сработала (#PF), страница только для чтения\n" : "ПРОШЛА - защиты нет\n");
    uart_print("[KIRA WX] итог: "); uart_print(now == 0 && early > 0 && t && r ? "ok\n" : "НАРУШЕНО\n");
}
