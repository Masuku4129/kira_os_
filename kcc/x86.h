/* kcc: внутренний интерфейс бэкенда x86-64 (кодировщик инструкций, кодогенератор, ELF). */
#ifndef KCC_X86_H
#define KCC_X86_H
#include "kcc.h"

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI, R_8, R_9, R_10, R_11, R_12, R_13, R_14, R_15 };

typedef struct XFix XFix;
struct XFix { XFix *next; int off; Obj *target; int64_t add; int line; };   /* rel32 до символа: S + add - (адрес off + 4) */

typedef struct { int off, lab; } XJmp;
typedef struct {
    unsigned char *p; size_t len, cap;
    XFix *fix, *fixtail;
    int *lab; int nlab;                       /* смещения меток текущей функции (-1 - не определена) */
    XJmp *jf; int njf, cjf;
    const char *file; int line;               /* для сообщений об отсутствующем шаблоне */
} XA;

enum { OP_NONE, OP_REG, OP_MEM, OP_IMM };
typedef struct { int k, sz, r; int64_t v; Obj *sym; } Opd;   /* MEM: [r + v] либо [rip + sym + v] */
#define NOOPD ((Opd){ OP_NONE, 0, 0, 0, NULL })
Opd xr(int sz, int r);
Opd xm(int sz, int base, int64_t disp);
Opd xs(int sz, Obj *sym, int64_t add);
Opd xi(int64_t v);

/* ALU-коды совпадают с x86 (базовый опкод = код * 8) */
enum { X_ADD = 0, X_OR = 1, X_AND = 4, X_SUB = 5, X_XOR = 6, X_CMP = 7,
       X_MOV = 16, X_MOVZX, X_MOVSX, X_LEA, X_TEST, X_IMUL, X_NEG, X_NOT, X_DIV, X_IDIV, X_SHL, X_SHR, X_SAR, X_CALLR,
       X_XCHG, X_LXADD, X_LCMPXCHG };   /* xchg [m], r; lock xadd [m], r; lock cmpxchg [m], r (размеры 1, 2, 4, 8) */
enum { X0_RET = 1, X0_CQO, X0_SYSCALL, X0_RDTSC, X0_MFENCE, X0_LFENCE, X0_SFENCE, X0_PAUSE, X0_HLT, X0_CLI, X0_STI, X0_NOP,
       X0_UD2, X0_REP_MOVSB, X0_REP_STOSB, X0_LEAVE, X0_PUSH_RBP, X0_IN8, X0_IN16, X0_IN32, X0_OUT8, X0_OUT16, X0_OUT32 };
enum { CC_E = 4, CC_NE = 5, CC_B = 2, CC_BE = 6, CC_L = 12, CC_LE = 14, CC_NONE = -1 };

void xa_ins(XA *a, int op, Opd d, Opd s);     /* нет шаблона для сочетания - ошибка компиляции */
void xa_op0(XA *a, int op);
void xa_setcc(XA *a, int cc, Opd d);
void xa_jmp(XA *a, int cc, int lab);          /* cc = CC_NONE: безусловный */
void xa_call(XA *a, Obj *target);
void xa_label(XA *a, int lab);
void xa_labels_begin(XA *a, int nlab);        /* метки 0..nlab */
void xa_labels_end(XA *a);                    /* разрешает переходы; неопределённая метка - ошибка */


#endif
