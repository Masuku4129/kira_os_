#ifndef KCCRT_SETJMP_H
#define KCCRT_SETJMP_H
/* x86-64: rbx, rbp, r12-r15, rsp, rip; ARM64: x19-x30, sp */
typedef unsigned long jmp_buf[13];
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
#endif
