/* Проверка кодировщика A64 (kcc/asm_a64.c): каждая строка "текст ассемблера | 32-битное слово" сверяется с эталоном tests/kcc/a64_ref.txt,
 * который получен ассемблером llvm-mc (--triple=aarch64) и хранится в репозитории. Режим "text" печатает только тексты (для получения эталона). */
#include "../../kcc/a64.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

KCtx K;
void *ka_new(size_t n) { return calloc(1, n); }
void kerr(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; (void)fmt; fprintf(stderr, "kerr\n"); exit(2); }
void sb_printf(SB *b, const char *f, ...) { (void)b; (void)f; }

static A6 A;
static int text_only;
#define T(txt, call) do { size_t s0 = A.len; call; \
    if (text_only) printf("%s\n", txt); \
    else for (size_t o = s0; o < A.len; o += 4) printf("%s|%02x%02x%02x%02x\n", txt, A.p[o + 3], A.p[o + 2], A.p[o + 1], A.p[o]); } while (0)

int main(int argc, char **argv) {
    text_only = argc > 1 && !strcmp(argv[1], "text");
    A.file = "t"; A.line = 1;
    T("movz x3, #0x1234", a6_movi(&A, 3, 0x1234));
    T("movz x3, #0x1234, lsl #16", a6_movi(&A, 3, 0x12340000));
    T("movn x3, #0x1233", a6_movi(&A, 3, ~0x1233LL));
    T("movz x4, #0", a6_movi(&A, 4, 0));
    T("movn x4, #0", a6_movi(&A, 4, -1));
    T("movz x5, #0x5678, lsl #32", a6_movi(&A, 5, 0x567800000000LL));
    T("mov x1, sp", a6_mov(&A, 1, A6_SP));
    T("mov sp, x2", a6_mov(&A, A6_SP, 2));
    T("mov x1, x2", a6_mov(&A, 1, 2));
    T("add x0, x1, x2", a6_arith(&A, A6_ADD, 0, 1, 2));
    T("sub x0, x1, x2", a6_arith(&A, A6_SUB, 0, 1, 2));
    T("adds x0, x1, x2", a6_arith(&A, A6_ADDS, 0, 1, 2));
    T("subs x0, x1, x2", a6_arith(&A, A6_SUBS, 0, 1, 2));
    T("cmp x9, x10", a6_arith(&A, A6_SUBS, A6_ZR, 9, 10));
    T("neg x0, x0", a6_arith(&A, A6_SUB, 0, A6_ZR, 0));
    T("add x17, sp, x17, uxtx", a6_addr(&A, 17, A6_SP, 17));
    T("add sp, sp, x16, uxtx", a6_addr(&A, A6_SP, A6_SP, 16));
    T("sub sp, sp, #32", a6_addi(&A, 1, A6_SP, A6_SP, 32));
    T("add x0, sp, #4095", a6_addi(&A, 0, 0, A6_SP, 4095));
    T("sub sp, sp, #1, lsl #12", a6_addi(&A, 1, A6_SP, A6_SP, 4096));
    T("add x29, sp, #8", a6_addi(&A, 0, 29, A6_SP, 8));
    T("cmp x3, #17", a6_cmpi(&A, 3, 17));
    T("and x0, x1, x2", a6_logic(&A, A6_AND, 0, 1, 2));
    T("orr x0, x1, x2", a6_logic(&A, A6_ORR, 0, 1, 2));
    T("eor x0, x1, x2", a6_logic(&A, A6_EOR, 0, 1, 2));
    T("ands x0, x1, x2", a6_logic(&A, A6_ANDS, 0, 1, 2));
    T("mvn x0, x1", a6_logic(&A, A6_ORN, 0, A6_ZR, 1));
    T("mul x0, x1, x2", a6_mul(&A, 0, 1, 2));
    T("msub x0, x1, x2, x3", a6_msub(&A, 0, 1, 2, 3));
    T("sdiv x0, x1, x2", a6_div(&A, 1, 0, 1, 2));
    T("udiv x0, x1, x2", a6_div(&A, 0, 0, 1, 2));
    T("lsl x0, x1, x2", a6_shv(&A, A6_LSLV, 0, 1, 2));
    T("lsr x0, x1, x2", a6_shv(&A, A6_LSRV, 0, 1, 2));
    T("asr x0, x1, x2", a6_shv(&A, A6_ASRV, 0, 1, 2));
    T("lsl x0, x1, #5", a6_shi(&A, A6_LSLV, 0, 1, 5));
    T("lsl x0, x1, #63", a6_shi(&A, A6_LSLV, 0, 1, 63));
    T("lsr x0, x1, #12", a6_shi(&A, A6_LSRV, 0, 1, 12));
    T("asr x0, x1, #33", a6_shi(&A, A6_ASRV, 0, 1, 33));
    T("sxtb x0, w1", a6_ext(&A, 0, 1, 1, 1));
    T("sxth x0, w1", a6_ext(&A, 0, 1, 2, 1));
    T("sxtw x0, w1", a6_ext(&A, 0, 1, 4, 1));
    T("uxtb w0, w1", a6_ext(&A, 0, 1, 1, 0));
    T("uxth w0, w1", a6_ext(&A, 0, 1, 2, 0));
    T("mov w0, w1", a6_ext(&A, 0, 1, 4, 0));
    T("ldr x0, [sp, #16]", a6_ls(&A, 1, 8, 0, 0, A6_SP, 16));
    T("str x3, [x4, #32760]", a6_ls(&A, 0, 8, 0, 3, 4, 32760));
    T("ldr w0, [x1, #8]", a6_ls(&A, 1, 4, 0, 0, 1, 8));
    T("ldrsw x0, [x1, #8]", a6_ls(&A, 1, 4, 1, 0, 1, 8));
    T("ldrh w0, [x1, #6]", a6_ls(&A, 1, 2, 0, 0, 1, 6));
    T("ldrsh x0, [x1, #6]", a6_ls(&A, 1, 2, 1, 0, 1, 6));
    T("ldrb w0, [x1, #7]", a6_ls(&A, 1, 1, 0, 0, 1, 7));
    T("ldrsb x0, [x1, #7]", a6_ls(&A, 1, 1, 1, 0, 1, 7));
    T("strb w0, [x1, #1]", a6_ls(&A, 0, 1, 0, 0, 1, 1));
    T("strh w0, [x1, #2]", a6_ls(&A, 0, 2, 0, 0, 1, 2));
    T("str w0, [x1]", a6_ls(&A, 0, 4, 0, 0, 1, 0));
    T("ldur x0, [x1, #-8]", a6_ls(&A, 1, 8, 0, 0, 1, -8));
    T("ldur x0, [x1, #3]", a6_ls(&A, 1, 8, 0, 0, 1, 3));
    T("stur w0, [x29, #-4]", a6_ls(&A, 0, 4, 0, 0, 29, -4));
    T("ldursb x0, [x1, #-1]", a6_ls(&A, 1, 1, 1, 0, 1, -1));
    T("ldr x0, [sp, #-8]", a6_ls(&A, 1, 8, 0, 0, A6_SP, -8));
    T("cset w0, eq", a6_cset(&A, 0, CC_EQ));
    T("cset w0, ne", a6_cset(&A, 0, CC_NE));
    T("cset w0, lo", a6_cset(&A, 0, CC_LO));
    T("cset w0, ls", a6_cset(&A, 0, CC_LS));
    T("cset w0, lt", a6_cset(&A, 0, CC_LT));
    T("cset w0, le", a6_cset(&A, 0, CC_LE));
    T("ldar x0, [x1]", a6_xmem(&A, X_LDAR, 8, 0, 1, 0));
    T("ldar w0, [x1]", a6_xmem(&A, X_LDAR, 4, 0, 1, 0));
    T("ldarh w0, [x1]", a6_xmem(&A, X_LDAR, 2, 0, 1, 0));
    T("ldarb w0, [x1]", a6_xmem(&A, X_LDAR, 1, 0, 1, 0));
    T("ldaxr x9, [x1]", a6_xmem(&A, X_LDAXR, 8, 9, 1, 0));
    T("ldaxrb w9, [x1]", a6_xmem(&A, X_LDAXR, 1, 9, 1, 0));
    T("ldaxrh w9, [x1]", a6_xmem(&A, X_LDAXR, 2, 9, 1, 0));
    T("ldaxr w9, [x1]", a6_xmem(&A, X_LDAXR, 4, 9, 1, 0));
    T("stlr x0, [x1]", a6_xmem(&A, X_STLR, 8, 0, 1, 0));
    T("stlrb w0, [x1]", a6_xmem(&A, X_STLR, 1, 0, 1, 0));
    T("stlxr w10, x11, [x1]", a6_xmem(&A, X_STLXR, 8, 11, 1, 10));
    T("stlxrb w10, w11, [x1]", a6_xmem(&A, X_STLXR, 1, 11, 1, 10));
    T("stlxrh w10, w11, [x1]", a6_xmem(&A, X_STLXR, 2, 11, 1, 10));
    T("stlxr w10, w11, [x1]", a6_xmem(&A, X_STLXR, 4, 11, 1, 10));
    T("nop", a6_op0(&A, Z_NOP));
    T("yield", a6_op0(&A, Z_YIELD));
    T("wfe", a6_op0(&A, Z_WFE));
    T("wfi", a6_op0(&A, Z_WFI));
    T("sev", a6_op0(&A, Z_SEV));
    T("svc #0", a6_op0(&A, Z_SVC0));
    T("brk #0", a6_op0(&A, Z_BRK0));
    T("isb", a6_op0(&A, Z_ISB));
    T("clrex", a6_op0(&A, Z_CLREX));
    T("ret", a6_op0(&A, Z_RET));
    T("dmb ish", a6_bar(&A, 0, BAR_ISH));
    T("dmb ishst", a6_bar(&A, 0, BAR_ISHST));
    T("dmb ishld", a6_bar(&A, 0, BAR_ISHLD));
    T("dmb sy", a6_bar(&A, 0, BAR_SY));
    T("dmb st", a6_bar(&A, 0, BAR_ST));
    T("dmb osh", a6_bar(&A, 0, BAR_OSH));
    T("dsb ish", a6_bar(&A, 1, BAR_ISH));
    T("dsb sy", a6_bar(&A, 1, BAR_SY));
    T("dsb st", a6_bar(&A, 1, BAR_ST));
    T("mrs x3, cntvct_el0", a6_mrs(&A, 3, SR_CNTVCT));
    T("mrs x3, cntfrq_el0", a6_mrs(&A, 3, SR_CNTFRQ));
    T("blr x15", a6_blr(&A, 15));
    /* переходы: метка 1 сразу после, метка 2 - перед (обратный переход) */
    a6_labels_begin(&A, 3);
    a6_label(&A, 2);
    size_t s0 = A.len;
    a6_b(&A, 1); a6_bcc(&A, CC_NE, 1); a6_cbz(&A, 0, 5, 1); a6_cbz(&A, 1, 5, 2); a6_cbzw(&A, 1, 10, 2); a6_cbzw(&A, 0, 10, 1); a6_b(&A, 2);
    a6_label(&A, 1);
    a6_labels_end(&A);
    static const char *jt[] = { "b .+28", "b.ne .+24", "cbz x5, .+20", "cbnz x5, .-12", "cbnz w10, .-16", "cbz w10, .+8", "b .-24" };
    for (int i = 0; i < 7; i++) {
        size_t o = s0 + 4 * (size_t)i;
        if (text_only) printf("%s\n", jt[i]);
        else printf("%s|%02x%02x%02x%02x\n", jt[i], A.p[o + 3], A.p[o + 2], A.p[o + 1], A.p[o]);
    }
    return 0;
}
