/* Порт MicroPython под Киру: WebAssembly (wasm32-wasi) в интерпретаторе userland/wasm.c. Файлы - плоская песочница /data/wasm (см. userland/wasi_host.h),
   ввод - строки консоли, часы и случайные числа - WASI. Сокетов, потоков и внешних библиотек нет. */
#include <stdint.h>
#include <alloca.h>

#define MICROPY_CONFIG_ROM_LEVEL            (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_NLR_SETJMP                  (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)
#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_FLOAT_IMPL                  (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL                (MICROPY_LONGINT_IMPL_MPZ)
#define MICROPY_PERSISTENT_CODE_LOAD        (1)
#define MICROPY_ENABLE_SOURCE_LINE          (1)
#define MICROPY_ERROR_REPORTING             (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_STACKLESS                   (0)
#define MICROPY_KBD_EXCEPTION               (0)
#define MICROPY_PY_SYS_PATH_ARGV_DEFAULTS   (0)

/* файловая система: VFS поверх POSIX-вызовов wasi-libc (WASI-файлы из wasi_host.h) */
#define MICROPY_VFS                         (1)
#define MICROPY_READER_VFS                  (1)
#define MICROPY_VFS_POSIX                   (1)
#define MICROPY_READER_POSIX                (1)
#define MICROPY_HELPER_LEXER_UNIX           (1)
#define MICROPY_EPOCH_IS_1970               (1)

/* модули */
#define MICROPY_PY_SYS                      (1)
#define MICROPY_PY_OS                       (1)
#define MICROPY_PY_OS_UNAME                 (0)
#define MICROPY_PY_TIME                     (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_JSON                     (1)
#define MICROPY_PY_RE                       (1)
#define MICROPY_PY_STRUCT                   (1)
#define MICROPY_PY_MATH                     (1)
#define MICROPY_PY_CMATH                    (1)
#define MICROPY_PY_COLLECTIONS              (1)
#define MICROPY_PY_IO                       (1)
#define MICROPY_PY_HASHLIB                  (1)
#define MICROPY_PY_BINASCII                 (1)
#define MICROPY_PY_RANDOM                   (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS       (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC    (mp_random_seed_init())
#define MICROPY_PY_ERRNO                    (1)
#define MICROPY_PY_GC                       (1)
#define MICROPY_PY_THREAD                   (0)
#define MICROPY_PY_SELECT                   (0)
#define MICROPY_PY_SOCKET                   (0)

typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

#define MICROPY_HW_BOARD_NAME               "kira-wasm"
#define MICROPY_HW_MCU_NAME                 "wasm32"
#define MICROPY_PY_SYS_PLATFORM             "kira"

/* куча сборщика: статическая, в линейной памяти модуля */
#ifndef KIRA_MPY_HEAP
#define KIRA_MPY_HEAP                       (3u << 20)
#endif

#include <stddef.h>
void mp_hal_get_random(size_t n, void *buf);
static inline unsigned long mp_random_seed_init(void) { unsigned long r; mp_hal_get_random(sizeof r, &r); return r; }

#define MP_STATE_PORT MP_STATE_VM
#define MICROPY_BEGIN_ATOMIC_SECTION()      (0)
#define MICROPY_END_ATOMIC_SECTION(x)       (void)(x)
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS)    mp_hal_delay_us(500)
#define MICROPY_EPOCH_IS_1970 (1)
#define MICROPY_PY_RE_MATCH_GROUPS (1)
#define MICROPY_PY_RE_MATCH_SPAN_START_END (1)
#define MICROPY_PY_RE_SUB (1)
#define MICROPY_STACK_CHECK (1)
