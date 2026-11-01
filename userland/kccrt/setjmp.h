#ifndef KCCRT_SETJMP_H
#define KCCRT_SETJMP_H
/* rbx, rbp, r12-r15, rsp, rip */
typedef unsigned long jmp_buf[8];
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#endif
