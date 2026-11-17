#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#define mp_hal_stdio_poll unused
#ifndef CHAR_CTRL_C
#define CHAR_CTRL_C (3)
#endif
void *mp_time_time_get(void);
#include <errno.h>
/* Повтор системного вызова при EINTR (PEP 475); потоков нет, поэтому GIL не нужен. */
#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) { \
        for (;;) { \
            ret = syscall; \
            if (ret == -1) { \
                int err = errno; \
                if (err == EINTR) { \
                    mp_handle_pending(true); \
                    continue; \
                } \
                raise; \
            } \
            break; \
        } \
    }
struct _vstr_t;
int mp_hal_readline(struct _vstr_t *line, const char *prompt);   /* input(): целая строка с консоли Киры (она сама печатает эхо) */
#define mp_hal_readline mp_hal_readline
