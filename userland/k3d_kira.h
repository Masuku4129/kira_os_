#ifndef K3D_KIRA_H
#define K3D_KIRA_H
/* Платформа Kira для движка k3d: потоки процесса (SYS_THREAD_CREATE, нужно право CAP_THREAD) и futex (SYS_FUTEX_*), часы. */
#include "kira.h"
#include "k3d.h"

static int k3d_kira_spawn(void (*fn)(k3d_u64), void* stack, k3d_u64 size, k3d_u64 arg) { return k_thread_create((void (*)(u64))fn, stack, size, arg) < 0 ? -1 : 0; }
static void k3d_kira_wait(volatile k3d_u32* w, k3d_u32 expected) { k_futex_wait(w, expected, K_FOREVER); }
static void k3d_kira_wake(volatile k3d_u32* w, k3d_u32 n) { k_futex_wake(w, n); }
static k3d_u64 k3d_kira_clock(void) { return k_clock_ns(); }
static inline void k3d_kira_sys(k3d_sys_t* s) { s->spawn = k3d_kira_spawn; s->futex_wait = k3d_kira_wait; s->futex_wake = k3d_kira_wake; s->clock_ns = k3d_kira_clock; }
#endif
