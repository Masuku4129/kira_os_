#ifndef KIRA_SYSCALL_SERVICES_H
#define KIRA_SYSCALL_SERVICES_H

#include <stdint.h>

/* Transitional in-kernel adapters; each subsystem owns its own syscall facade. */
int syscall_service_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result);
int syscall_service_graphics(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result);
int syscall_service_filesystem(uint64_t nr, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t* result);
int syscall_service_network(uint64_t nr, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t* result);
int syscall_service_audio(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t* result);
int syscall_service_clock(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t* result);

#endif
