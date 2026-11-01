/* kcc: внутренний интерфейс бэкенда AArch64 (кодировщик инструкций A64, кодогенератор). */
#ifndef KCC_A64_H
#define KCC_A64_H
#include "kcc.h"

/* Регистры x0..x30; номер 31 - sp либо xzr (по форме инструкции, см. комментарии у функций). */
enum { A6_SP = 31, A6_ZR = 31, A6_FP = 29, A6_LR = 30 };
/* Условия */
enum { CC_EQ = 0, CC_NE = 1, CC_HS = 2, CC_LO = 3, CC_HI = 8, CC_LS = 9, CC_GE = 10, CC_LT = 11, CC_GT = 12, CC_LE = 13 };
/* Арифметика со сдвигом регистра / логика */
enum { A6_ADD, A6_SUB, A6_ADDS, A6_SUBS };
enum { A6_AND, A6_ORR, A6_EOR, A6_ANDS, A6_ORN };
enum { A6_LSLV, A6_LSRV, A6_ASRV };
/* Инструкции без операндов */
enum { Z_NOP, Z_YIELD, Z_WFE, Z_WFI, Z_SEV, Z_SVC0, Z_BRK0, Z_ISB, Z_CLREX, Z_RET };
/* Барьеры: операнд CRm */
enum { BAR_OSH = 3, BAR_ISHLD = 9, BAR_ISHST = 10, BAR_ISH = 11, BAR_ST = 14, BAR_LD = 13, BAR_SY = 15 };
/* Исключающий доступ: LDAR, LDAXR, STLR, STLXR */
enum { X_LDAR, X_LDAXR, X_STLR, X_STLXR };
/* Системные регистры, читаемые mrs */
enum { SR_CNTVCT, SR_CNTFRQ };

typedef struct A6Fix A6Fix;
struct A6Fix { A6Fix *next; int off; Obj *target; int64_t add; int line, kind; };   /* kind: 0 ADRP, 1 ADD lo12, 2 BL */
typedef struct { int off, lab, kind; } A6Jmp;                                      /* kind: 0 B, 1 условная/CBZ (imm19) */
typedef struct {
    unsigned char *p; size_t len, cap;
    A6Fix *fix, *fixtail;
    int *lab; int nlab;                      /* смещения меток (в байтах), -1 - не определена */
    A6Jmp *jf; int njf, cjf;
    const char *file; int line;
} A6;

void a6_w(A6 *a, uint32_t w);
void a6_movi(A6 *a, int rd, int64_t v);                   /* movz/movn/movk, 1..4 инструкции */
void a6_mov(A6 *a, int rd, int rm);                       /* rd, rm могут быть sp */
void a6_arith(A6 *a, int op, int rd, int rn, int rm);     /* 64-битное со сдвигом по номеру; 31 = xzr */
void a6_addr(A6 *a, int rd, int rn, int rm);              /* rd = rn + rm (rd, rn могут быть sp) */
void a6_addi(A6 *a, int sub, int rd, int rn, uint32_t imm);  /* rd, rn могут быть sp; imm < 2^24 */
void a6_cmpi(A6 *a, int rn, uint32_t imm);                /* subs xzr, rn, #imm (imm < 4096) */
void a6_logic(A6 *a, int op, int rd, int rn, int rm);
void a6_mul(A6 *a, int rd, int rn, int rm);
void a6_div(A6 *a, int sgn, int rd, int rn, int rm);
void a6_msub(A6 *a, int rd, int rn, int rm, int ra);      /* rd = ra - rn * rm */
void a6_shv(A6 *a, int kind, int rd, int rn, int rm);
void a6_shi(A6 *a, int kind, int rd, int rn, int n);      /* kind: A6_LSLV / A6_LSRV / A6_ASRV, 0 <= n < 64 */
void a6_ext(A6 *a, int rd, int rn, int w, int sgn);       /* расширение младших w (1, 2, 4) байт до 64 бит */
void a6_ls(A6 *a, int load, int size, int sgnext, int rt, int rn, int64_t off);   /* size 1, 2, 4, 8; sgnext: знаковое расширение до 64 */
void a6_cset(A6 *a, int rd, int cc);
void a6_xmem(A6 *a, int kind, int size, int rt, int rn, int rs);   /* rs - статус для STLXR */
void a6_op0(A6 *a, int op);
void a6_bar(A6 *a, int dsb, int crm);
void a6_mrs(A6 *a, int rt, int sr);
void a6_blr(A6 *a, int rn);

void a6_labels_begin(A6 *a, int nlab);
void a6_label(A6 *a, int lab);
void a6_b(A6 *a, int lab);
void a6_bcc(A6 *a, int cc, int lab);
void a6_cbz(A6 *a, int nz, int rt, int lab);
void a6_cbzw(A6 *a, int nz, int rt, int lab);             /* 32-битная форма */
void a6_bl(A6 *a, Obj *t);
void a6_gaddr(A6 *a, int rd, Obj *t, int64_t add);
void a6_labels_end(A6 *a);

#endif
