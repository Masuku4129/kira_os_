/* Проверка SSE-кодировок kcc/asm_x86.c: каждая строка "текст ассемблера AT&T | байты" сверяется с эталоном tests/kcc/x86_sse_ref.txt,
 * полученным llvm-mc (--triple=x86_64 --show-encoding) и хранимым в репозитории. Режим "text" печатает только тексты (для получения эталона). */
#include "../../kcc/x86.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KCtx K;
void *ka_new(size_t n) { return calloc(1, n); }
void kerr(const char *file, int line, const char *fmt, ...) { (void)file; (void)line; (void)fmt; fprintf(stderr, "kerr\n"); exit(2); }

static XA A;
static int text_only;
#define T(txt, op, d, s) do { size_t s0 = A.len; xa_ins(&A, op, d, s); \
    if (text_only) printf("%s\n", txt); \
    else { printf("%s|", txt); for (size_t o = s0; o < A.len; o++) printf("%02x", A.p[o]); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    text_only = argc > 1 && !strcmp(argv[1], "text");
    A.file = "t"; A.line = 1;
    T("movss %xmm1, %xmm2", X_MOVS, xx(4, 2), xx(4, 1));
    T("movsd %xmm1, %xmm2", X_MOVS, xx(8, 2), xx(8, 1));
    T("movsd %xmm9, %xmm12", X_MOVS, xx(8, 12), xx(8, 9));
    T("movss (%rax), %xmm3", X_MOVS, xx(4, 3), xm(4, R_AX, 0));
    T("movsd -8(%rbp), %xmm0", X_MOVS, xx(8, 0), xm(8, R_BP, -8));
    T("movsd 1000(%r12), %xmm10", X_MOVS, xx(8, 10), xm(8, R_12, 1000));
    T("movsd (%r13), %xmm8", X_MOVS, xx(8, 8), xm(8, R_13, 0));
    T("movsd (%rsp), %xmm1", X_MOVS, xx(8, 1), xm(8, R_SP, 0));
    T("movsd 16(%rsp), %xmm15", X_MOVS, xx(8, 15), xm(8, R_SP, 16));
    T("movss %xmm4, (%rdi)", X_MOVS, xm(4, R_DI, 0), xx(4, 4));
    T("movsd %xmm13, -24(%rbp)", X_MOVS, xm(8, R_BP, -24), xx(8, 13));
    T("movsd %xmm2, (%rax,%rcx,8)", X_MOVS, xmi(8, R_AX, R_CX, 3, 0), xx(8, 2));
    T("movss 4(%r8,%r9,4), %xmm5", X_MOVS, xx(4, 5), xmi(4, R_8, R_9, 2, 4));
    T("movsd (%rbx,%r15,8), %xmm11", X_MOVS, xx(8, 11), xmi(8, R_BX, R_15, 3, 0));
    T("movaps %xmm1, %xmm2", X_MOVAPS, xx(8, 2), xx(8, 1));
    T("movaps %xmm14, %xmm8", X_MOVAPS, xx(8, 8), xx(8, 14));
    T("movups %xmm3, -176(%rbp)", X_MOVUPS, xm(8, R_BP, -176), xx(8, 3));
    T("movups %xmm9, -64(%rbp)", X_MOVUPS, xm(8, R_BP, -64), xx(8, 9));
    T("movups (%rax), %xmm7", X_MOVUPS, xx(8, 7), xm(8, R_AX, 0));
    T("addss %xmm1, %xmm0", X_ADDS, xx(4, 0), xx(4, 1));
    T("addsd %xmm1, %xmm0", X_ADDS, xx(8, 0), xx(8, 1));
    T("addsd %xmm12, %xmm8", X_ADDS, xx(8, 8), xx(8, 12));
    T("subsd -16(%rbp), %xmm0", X_SUBS, xx(8, 0), xm(8, R_BP, -16));
    T("subss %xmm9, %xmm2", X_SUBS, xx(4, 2), xx(4, 9));
    T("mulsd %xmm3, %xmm10", X_MULS, xx(8, 10), xx(8, 3));
    T("mulss (%rdx), %xmm1", X_MULS, xx(4, 1), xm(4, R_DX, 0));
    T("divsd %xmm1, %xmm0", X_DIVS, xx(8, 0), xx(8, 1));
    T("divss %xmm15, %xmm14", X_DIVS, xx(4, 14), xx(4, 15));
    T("sqrtsd %xmm2, %xmm3", X_SQRTS, xx(8, 3), xx(8, 2));
    T("sqrtss %xmm0, %xmm0", X_SQRTS, xx(4, 0), xx(4, 0));
    T("sqrtsd (%rax), %xmm5", X_SQRTS, xx(8, 5), xm(8, R_AX, 0));
    T("ucomiss %xmm1, %xmm0", X_UCOMIS, xx(4, 0), xx(4, 1));
    T("ucomisd %xmm1, %xmm0", X_UCOMIS, xx(8, 0), xx(8, 1));
    T("ucomisd %xmm13, %xmm9", X_UCOMIS, xx(8, 9), xx(8, 13));
    T("ucomisd 8(%rbp), %xmm2", X_UCOMIS, xx(8, 2), xm(8, R_BP, 8));
    T("cvtsi2sdq %rax, %xmm0", X_CVTSI2S, xx(8, 0), xr(8, R_AX));
    T("cvtsi2sdl %eax, %xmm0", X_CVTSI2S, xx(8, 0), xr(4, R_AX));
    T("cvtsi2ssq %rcx, %xmm1", X_CVTSI2S, xx(4, 1), xr(8, R_CX));
    T("cvtsi2ssl %r9d, %xmm10", X_CVTSI2S, xx(4, 10), xr(4, R_9));
    T("cvtsi2sdq %r12, %xmm13", X_CVTSI2S, xx(8, 13), xr(8, R_12));
    T("cvtsi2sdq -8(%rbp), %xmm1", X_CVTSI2S, xx(8, 1), xm(8, R_BP, -8));
    T("cvttsd2siq %xmm0, %rax", X_CVTTS2SI, xr(8, R_AX), xx(8, 0));
    T("cvttsd2sil %xmm0, %eax", X_CVTTS2SI, xr(4, R_AX), xx(8, 0));
    T("cvttss2siq %xmm1, %rcx", X_CVTTS2SI, xr(8, R_CX), xx(4, 1));
    T("cvttss2sil %xmm11, %r10d", X_CVTTS2SI, xr(4, R_10), xx(4, 11));
    T("cvttsd2siq (%rax), %rdx", X_CVTTS2SI, xr(8, R_DX), xm(8, R_AX, 0));
    T("cvtss2sd %xmm1, %xmm0", X_CVTS2S, xx(8, 0), xx(4, 1));
    T("cvtsd2ss %xmm1, %xmm0", X_CVTS2S, xx(4, 0), xx(8, 1));
    T("cvtss2sd %xmm9, %xmm14", X_CVTS2S, xx(8, 14), xx(4, 9));
    T("cvtsd2ss (%rax), %xmm2", X_CVTS2S, xx(4, 2), xm(8, R_AX, 0));
    T("movq %rax, %xmm0", X_MOVGX, xx(8, 0), xr(8, R_AX));
    T("movq %r12, %xmm9", X_MOVGX, xx(8, 9), xr(8, R_12));
    T("movd %eax, %xmm1", X_MOVGX, xx(4, 1), xr(4, R_AX));
    T("movd %r10d, %xmm15", X_MOVGX, xx(4, 15), xr(4, R_10));
    T("movq %xmm0, %rax", X_MOVXG, xr(8, R_AX), xx(8, 0));
    T("movq %xmm9, %rbx", X_MOVXG, xr(8, R_BX), xx(8, 9));
    T("movd %xmm1, %eax", X_MOVXG, xr(4, R_AX), xx(4, 1));
    T("movd %xmm15, %r11d", X_MOVXG, xr(4, R_11), xx(4, 15));
    T("xorps %xmm0, %xmm0", X_XORPS, xx(8, 0), xx(8, 0));
    T("xorps %xmm15, %xmm13", X_XORPS, xx(8, 13), xx(8, 15));
    T("xorps (%rax), %xmm1", X_XORPS, xx(8, 1), xm(8, R_AX, 0));
    T("andnps %xmm1, %xmm14", X_ANDNPS, xx(8, 14), xx(8, 1));
    T("movd (%rax), %xmm2", X_MOVGX, xx(4, 2), xm(4, R_AX, 0));
    return 0;
}
