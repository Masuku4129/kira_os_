#ifndef FPU_H
#define FPU_H
#include <stdint.h>
/* ARM64: состояние SIMD/FP потока - 32 регистра по 16 байт, затем FPCR и FPSR. */
#define FPU_STATE_SIZE 528
const void* fpu_initial_state(void);
#endif
