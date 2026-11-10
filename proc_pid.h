#ifndef KIRA_PROC_PID_H
#define KIRA_PROC_PID_H
#include <stdint.h>

/* PIDs are security principals for private IPC ownership. Never issue PID 0,
   a value that cannot be returned through the signed spawn API, or reuse a PID
   after exhaustion: reuse could grant a new process an old process's objects. */
static inline uint32_t proc_pid_take(uint32_t *next_pid) {
    if (!next_pid || *next_pid == 0 || *next_pid > 0x7fffffffU)
        return 0;
    return (*next_pid)++;
}

#endif
