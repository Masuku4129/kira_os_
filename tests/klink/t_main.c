/* Программа Киры из нескольких объектов (clang, nasm): данные с указателями, bss, слабые и общие символы, GOT-доступ.
   Печатает контрольные строки через системный вызов Киры; вывод сверяется с t_expected.txt и с ld.lld. */
#include "kira.h"
KIRA_ENTRY()

extern int add3(int);
extern const char *const names[];
extern int nnames;
extern int (*const ops[])(int);
extern int pick(void);
extern int bump(void);
extern int asm_twice(int);
extern int (*asm_fn)(int);
extern int *got_addr(void), *got_addr2(void);
extern int got_twice(int);
extern int got_is(int *);
extern int data_val;
extern int cm_user(void), cm_user2(void);
extern int got_probe(void), got_lazy_ok(void);
extern int weak_probe(void);

static void put(const char *s) { k_write(s, k_strlen(s)); }
static void num(const char *label, i64 v) {
    char b[24];
    int n = 0;
    u64 u = v < 0 ? (u64)(-v) : (u64)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    put(label);
    put("=");
    if (v < 0) put("-");
    while (n) k_write(&b[--n], 1);
    put("\n");
}

int umain(void) {
    num("add3", add3(4));
    for (int i = 0; i < nnames; i++) put(names[i]), put(i + 1 < nnames ? "," : "\n");
    num("sq", ops[0](9));
    num("neg", ops[1](9));
    num("ops2", ops[2](9));
    num("pick", pick());
    num("bump", bump());
    num("bump", bump());
    num("asm_twice", asm_twice(21));
    num("asm_fn", asm_fn(5));
    num("cm", cm_user());
    num("cm", cm_user2());
    num("got", got_probe());
    num("got_ok", got_lazy_ok());
    num("got_addr", *got_addr());
    num("got_same", got_is(&data_val) && got_addr2() == got_addr());
    num("got_twice", got_twice(21));
    num("weak", weak_probe());
    return 0;
}
