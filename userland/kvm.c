/* KIRA VM v1 userland runner.
 * This is a deliberately small, fail-closed first OS integration:
 * one granted module (file grant or explicit path), private memory, bounded
 * module/guest quotas, and only console/tick/yield host capabilities.
 */
#include "kira.h"
#include "../kvm/kvm.h"

#define RUNNER_MAX_MODULE_BYTES (1024u * 1024u)
#define RUNNER_MAX_MEMORY_PAGES 128u
#define RUNNER_FUEL 10000000ull
#define RUNNER_WRITE_MAX 4096u

enum {
    KVM_CAP_CONSOLE = 0,
    KVM_CAP_TICKS = 1,
    KVM_CAP_YIELD = 2
};
enum {
    KVM_HOST_WRITE = 0,
    KVM_HOST_TICKS = 1,
    KVM_HOST_YIELD = 2
};

typedef struct {
    struct kvm_instance *instance;
} runner_host_t;

static void say(const char *s)
{
    k_puts(s);
}

static void say_number(u64 value)
{
    char text[21];
    unsigned at = sizeof(text) - 1u;
    text[at] = 0;
    do {
        text[--at] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    k_write(text + at, sizeof(text) - 1u - at);
}

static void *runner_allocate(void *context, size_t bytes)
{
    u64 pages;
    (void)context;
    if (!bytes || (u64)bytes > 4u * 1024u * 1024u)
        return 0;
    pages = ((u64)bytes + KVM_PAGE_SIZE - 1u) / KVM_PAGE_SIZE;
    return k_heap_grow(pages);
}

static void runner_release(void *context, void *memory, size_t bytes)
{
    volatile u8 *p = (volatile u8 *)memory;
    size_t i;
    (void)context;
    for (i = 0; p && i < bytes; ++i)
        p[i] = 0;
}

static int runner_syscall(void *context, uint32_t number, uint8_t capability,
                          const uint64_t args[4], uint64_t *result)
{
    runner_host_t *host = (runner_host_t *)context;
    struct kvm_instance *vm;
    if (!host || !host->instance || !args || !result)
        return -1;
    vm = host->instance;

    if (number == KVM_HOST_WRITE && capability == KVM_CAP_CONSOLE) {
        if (args[0] > vm->memory_size ||
            args[1] > vm->memory_size - (size_t)args[0] ||
            args[1] > RUNNER_WRITE_MAX)
            return -1;
        if (args[1])
            k_write((const char *)vm->memory + (size_t)args[0], args[1]);
        *result = args[1];
        return 0;
    }
    if (number == KVM_HOST_TICKS && capability == KVM_CAP_TICKS) {
        *result = k_ticks();
        return 0;
    }
    if (number == KVM_HOST_YIELD && capability == KVM_CAP_YIELD) {
        k_yield();
        *result = 0;
        return 0;
    }
    return -1;
}

static int read_module(const char *path, uint8_t **blob, size_t *length)
{
    i64 size;
    u64 pages;
    void *memory;
    size_t name_length, i;
    if (!path || !*path || !blob || !length)
        return -1;
    name_length = k_strlen(path);
    if (name_length > 63u || (name_length == 1u && path[0] == '.') ||
        (name_length == 2u && path[0] == '.' && path[1] == '.'))
        return -1;
    /* KFS_READ is intentionally scoped to this runner's sandbox; files outside
       it must be passed as a file-manager grant, never by a forged path. */
    for (i = 0; i < name_length; ++i) {
        char c = path[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
            return -1;
    }
    size = k_file_size(path);
    if (size <= 0 || (u64)size > RUNNER_MAX_MODULE_BYTES)
        return -1;
    pages = ((u64)size + KVM_PAGE_SIZE - 1u) / KVM_PAGE_SIZE;
    memory = k_heap_grow(pages);
    if (!memory)
        return -1;
    if (k_file_read(path, memory, (u64)size, 0) != size)
        return -1;
    *blob = (uint8_t *)memory;
    *length = (size_t)size;
    return 0;
}

static int read_granted_module(uint8_t **blob, size_t *length)
{
    i64 size = k_grant_size();
    u64 pages;
    void *memory;
    if (size <= 0 || (u64)size > RUNNER_MAX_MODULE_BYTES)
        return -1;
    pages = ((u64)size + KVM_PAGE_SIZE - 1u) / KVM_PAGE_SIZE;
    memory = k_heap_grow(pages);
    if (!memory)
        return -1;
    if (k_grant_read(memory, (u64)size, 0) != size)
        return -1;
    *blob = (uint8_t *)memory;
    *length = (size_t)size;
    return 0;
}

static int get_path(char *path, size_t capacity)
{
    i64 n = k_args(path, capacity);
    size_t begin = 0, end;
    if (n <= 0 || (u64)n >= capacity)
        return 0;
    path[n] = 0;
    while (path[begin] == ' ' || path[begin] == '\t')
        ++begin;
    end = begin;
    while (path[end] && path[end] != ' ' && path[end] != '\t' &&
           path[end] != '\r' && path[end] != '\n')
        ++end;
    if (end == begin)
        return 0;
    if (path[end]) {
        size_t tail = end;
        while (path[tail] == ' ' || path[tail] == '\t' ||
               path[tail] == '\r' || path[tail] == '\n')
            ++tail;
        if (path[tail])
            return -1; /* one module path only; no ambiguous argument parsing */
    }
    path[end] = 0;
    if (begin)
        for (size_t i = 0; path[begin + i]; ++i)
            path[i] = path[begin + i];
    return 1;
}

int umain(void)
{
    char path[256];
    uint8_t *blob = 0;
    size_t blob_size = 0;
    struct kvm_module module;
    struct kvm_instance instance;
    struct kvm_allocator allocator = { 0, runner_allocate, runner_release };
    runner_host_t host_context = { 0 };
    struct kvm_host host = { &host_context, runner_syscall };
    const uint64_t grants = (1ull << KVM_CAP_CONSOLE) |
                            (1ull << KVM_CAP_TICKS) |
                            (1ull << KVM_CAP_YIELD);
    uint64_t exit_value = 0;
    int path_state, rc;

    path_state = get_path(path, sizeof(path));
    if (path_state < 0) {
        say("KIRA VM: укажите одно имя .kvm в песочнице или откройте файл через файловый менеджер\n");
        return 2;
    }
    if (path_state > 0) {
        if (read_module(path, &blob, &blob_size)) {
            say("KIRA VM: не удалось прочитать файл песочницы или превышен лимит 1 MiB\n");
            return 1;
        }
    } else {
        if (read_granted_module(&blob, &blob_size)) {
            say("KIRA VM: откройте .kvm через файловый менеджер или передайте путь аргументом\n");
            return 1;
        }
    }

    rc = kvm_module_decode(blob, blob_size, &module);
    if (rc != KVM_OK) {
        say("KIRA VM: повреждённый контейнер KVM1, код ");
        say_number((u64)(-rc));
        say("\n");
        return 1;
    }
    if (module.memory_pages > RUNNER_MAX_MEMORY_PAGES) {
        say("KIRA VM: превышен лимит памяти runner (512 KiB)\n");
        return 1;
    }
    rc = kvm_verify(&module, grants);
    if (rc != KVM_OK) {
        say("KIRA VM: модуль не прошёл проверку, код ");
        say_number((u64)(-rc));
        say("\n");
        return 1;
    }

    rc = kvm_instance_init(&instance, &module, grants, &allocator, &host);
    if (rc != KVM_OK) {
        say("KIRA VM: не удалось создать изолированный экземпляр, код ");
        say_number((u64)(-rc));
        say("\n");
        return 1;
    }
    host_context.instance = &instance;
    say("KIRA VM: запуск KVM1; лимит памяти 512 KiB; fuel 10000000\n");
    rc = kvm_run(&instance, RUNNER_FUEL, &exit_value);
    kvm_instance_destroy(&instance);
    say("KIRA VM: завершение, статус ");
    say_number((u64)(rc == KVM_OK ? 0 : -rc));
    say(", значение ");
    say_number(exit_value);
    say("\n");
    return rc == KVM_OK ? 0 : 1;
}

KIRA_ENTRY()
