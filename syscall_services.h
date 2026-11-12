#ifndef KIRA_SYSCALL_SERVICES_H
#define KIRA_SYSCALL_SERVICES_H

#include <stdint.h>

/* Transitional in-kernel adapters. These are not yet isolated service processes. */
int syscall_service_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result);

#endif
