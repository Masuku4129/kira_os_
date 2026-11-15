/* Python (MicroPython) для Киры: точка входа и связь с WASI (ввод строк, вывод, часы, случайные числа).
   Запуск из консоли Киры: «python» - REPL (строка за строкой: ввод построчный, эхо делает консоль), «python файл.py [аргументы]»,
   «python -c "код"». Файлы читаются из песочницы /data/wasm (плоской: без подкаталогов, имя [A-Za-z0-9._-]). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "py/builtin.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"

static char heap[KIRA_MPY_HEAP];

/* ---- HAL ---- */
mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    size_t done = 0;
    while (done < len) { ssize_t n = write(1, str + done, len - done); if (n <= 0) break; done += (size_t)n; }
    return (mp_uint_t)done;
}
int mp_hal_stdin_rx_chr(void) { unsigned char c; ssize_t n = read(0, &c, 1); return n == 1 ? c : -1; }
mp_uint_t mp_hal_ticks_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (mp_uint_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000); }
mp_uint_t mp_hal_ticks_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (mp_uint_t)(t.tv_sec * 1000000 + t.tv_nsec / 1000); }
mp_uint_t mp_hal_ticks_cpu(void) { return mp_hal_ticks_us(); }
uint64_t mp_hal_time_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void mp_hal_delay_us(mp_uint_t us) { struct timespec t = { us / 1000000, (long)(us % 1000000) * 1000 }; nanosleep(&t, NULL); }
void mp_hal_delay_ms(mp_uint_t ms) { mp_hal_delay_us(ms * 1000); }
void mp_hal_get_random(size_t n, void *buf) { if (getentropy(buf, n) != 0) { mp_raise_OSError(MP_EIO); } }
void mp_hal_set_interrupt_char(char c) { (void)c; }
void nlr_jump_fail(void *val) { (void)val; write(2, "nlr_jump_fail\n", 14); _exit(1); }
#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    (void)func; printf("Assertion '%s' failed, at file %s:%d\n", expr, file, line); _exit(1);
}
#endif

/* ---- сборщик мусора ----
   Корни: стек C в линейной памяти модуля и все живые локальные и операнды WASM (указатель может жить только в локальной переменной, в линейной
   памяти его тогда нет): их отдаёт интерпретатор вызовом spill (userland/wasm.c). */
__attribute__((import_module("kira"), import_name("spill"))) int kira_spill(void *buf, int cap_words);
static char *stack_top;
static uint32_t spill_buf[2 * 64 * 1024];                       /* по два слова на значение; стек значений интерпретатора - до 64 К значений */
void gc_collect(void) {
    gc_collect_start();
    void *dummy;
    gc_collect_root(&dummy, ((mp_uint_t)stack_top - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    int n = kira_spill(spill_buf, (int)(sizeof spill_buf / sizeof spill_buf[0]));
    if (n < 0) { write(2, "gc: spill overflow\n", 19); _exit(1); }
    gc_collect_root((void **)spill_buf, (size_t)n);
    gc_collect_end();
}

/* Необработанное исключение: SystemExit (sys.exit) - код выхода модуля, остальное - трассировка и код 1. */
static int g_exit = -1;
static int unhandled(mp_obj_t exc) {
    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(mp_obj_get_type(exc)), MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
        mp_obj_t v = mp_obj_exception_get_value(exc); int c = 0;
        if (v == mp_const_none) c = 0;
        else if (mp_obj_is_small_int(v)) c = (int)MP_OBJ_SMALL_INT_VALUE(v);
        else { mp_obj_print(v, PRINT_STR); mp_hal_stdout_tx_str("\n"); c = 1; }
        g_exit = c; return c;
    }
    mp_obj_print_exception(&mp_plat_print, exc);
    return 1;
}

/* Выполнить строку или файл, напечатать необработанное исключение. 0 - успех. */
static int run_str(const char *src, mp_parse_input_kind_t kind, qstr source_name) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(source_name, src, strlen(src), 0);
        mp_parse_tree_t pt = mp_parse(lex, kind);
        mp_obj_t fn = mp_compile(&pt, source_name, kind == MP_PARSE_SINGLE_INPUT);
        mp_call_function_0(fn);
        nlr_pop();
        return 0;
    }
    return unhandled((mp_obj_t)nlr.ret_val);
}
static int run_file(const char *path) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_file(qstr_from_str(path));
        qstr name = lex->source_name;
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t fn = mp_compile(&pt, name, false);
        mp_call_function_0(fn);
        nlr_pop();
        return 0;
    }
    return unhandled((mp_obj_t)nlr.ret_val);
}

/* REPL построчно: консоль Киры отдаёт целые строки и сама печатает эхо, поэтому readline MicroPython (поштучный ввод с эхо) не нужен.
   Составные операторы (def, for, if...) продолжаются строками «... » до пустой строки, как в обычном Python. */
static int line_read(char *buf, size_t cap) {
    size_t n = 0;
    for (;;) {
        unsigned char c; ssize_t r = read(0, &c, 1);
        if (r <= 0) return n ? (int)n : -1;                      /* конец ввода (Ctrl+D) или прерывание (Ctrl+C) */
        if (c == '\n') return (int)n;
        if (n + 1 < cap) buf[n++] = (char)c;
    }
}
static void repl(void) {
    static char src[4096];
    mp_hal_stdout_tx_str("MicroPython v1.24.1 на Кире (WebAssembly). Ctrl+D - выход.\n");
    for (;;) {
        size_t len = 0; src[0] = 0;
        mp_hal_stdout_tx_str(">>> ");
        for (;;) {
            char line[512]; int n = line_read(line, sizeof line);
            if (n < 0) { mp_hal_stdout_tx_str("\n"); return; }
            if (len + (size_t)n + 2 >= sizeof src) { mp_hal_stdout_tx_str("строка слишком длинная\n"); len = 0; src[0] = 0; break; }
            /* как в обычном REPL MicroPython: строки через \n без хвостового \n; пустая последняя строка даёт конец на \n и закрывает блок */
            if (len) src[len++] = '\n';
            memcpy(src + len, line, (size_t)n); len += (size_t)n; src[len] = 0;
            if (len == 0) break;                                             /* пустая строка на первом приглашении */
            if (!mp_repl_continue_with_input(src)) { run_str(src, MP_PARSE_SINGLE_INPUT, MP_QSTR__lt_stdin_gt_); if (g_exit >= 0) return; break; }
            mp_hal_stdout_tx_str("... ");
        }
    }
}

int main(int argc, char **argv) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;
    mp_stack_set_top(stack_top);
    mp_stack_set_limit(60 * 1024);                               /* стек модуля 64 КБ (см. сборку): запас на ловушку переполнения */
    gc_init(heap, heap + sizeof heap);
    mp_init();

    {   /* корень VFS - песочница модуля («/» WASI) */
        mp_obj_t a[2] = { MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(&mp_type_vfs_posix, 0, 0, NULL), MP_OBJ_NEW_QSTR(MP_QSTR__slash_) };
        mp_vfs_mount(2, a, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
    }
    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_));
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);

    int rc = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-c") == 0) {
        if (i + 1 >= argc) { mp_hal_stdout_tx_str("python: после -c нужен код\n"); rc = 2; }
        else {
            mp_obj_list_append(mp_sys_argv, mp_obj_new_str("-c", 2));
            for (int k = i + 2; k < argc; k++) mp_obj_list_append(mp_sys_argv, mp_obj_new_str(argv[k], strlen(argv[k])));
            rc = run_str(argv[i + 1], MP_PARSE_FILE_INPUT, MP_QSTR__lt_string_gt_);
        }
    } else if (i < argc) {
        for (int k = i; k < argc; k++) mp_obj_list_append(mp_sys_argv, mp_obj_new_str(argv[k], strlen(argv[k])));
        rc = run_file(argv[i]);
    } else {
        mp_obj_list_append(mp_sys_argv, MP_OBJ_NEW_QSTR(MP_QSTR_));
        repl();
    }
    mp_deinit();
    return g_exit >= 0 ? g_exit : rc;
}

/* time.time(): секунды с 1970 года как число с плавающей точкой (часы реального времени берутся у хоста через WASI). */
void *mp_time_time_get(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return mp_obj_new_float((mp_float_t)t.tv_sec + (mp_float_t)t.tv_nsec / 1e9);
}

int mp_hal_readline(vstr_t *line, const char *prompt) {
    (void)prompt;
    for (;;) {
        unsigned char c; ssize_t r = read(0, &c, 1);
        if (r <= 0) return 4;                                    /* конец ввода: CHAR_CTRL_D */
        if (c == '\n') return 0;
        vstr_add_byte(line, c);
    }
}
