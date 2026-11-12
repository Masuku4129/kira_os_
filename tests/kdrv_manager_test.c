#include "drivers/kdrv/kdrv_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char bytes[16];
    int signature_ok;
    int dependency_ok;
    int stage_fail;
    int commit_fail;
    int bad_name;
    int bad_version;
    int bad_dependency;
    int staged;
    int committed;
    int discarded;
    int freed;
    enum kdrv_state states[8];
    unsigned state_count;
} test_context_t;

static int read_package(void *opaque, const char *path, void **data, size_t *size)
{
    test_context_t *ctx = opaque;
    unsigned char *copy;
    if (strcmp(path, "/drv/test.kdrv") != 0) return -1;
    copy = malloc(sizeof ctx->bytes);
    if (!copy) return -1;
    memcpy(copy, ctx->bytes, sizeof ctx->bytes);
    *data = copy;
    *size = sizeof ctx->bytes;
    return 0;
}

static void free_package(void *opaque, void *data, size_t size)
{
    test_context_t *ctx = opaque;
    if (size != sizeof ctx->bytes) abort();
    ctx->freed++;
    free(data);
}

static int parse_manifest(void *opaque, const void *package, size_t size,
                          struct kdrv_manifest *manifest)
{
    test_context_t *ctx = opaque;
    if (!package || size != 16) return -1;
    memset(manifest, 0, sizeof *manifest);
    strcpy(manifest->name, "test-driver");
    strcpy(manifest->version, "1.2.3");
    strcpy(manifest->target_arch, "x86_64");
    manifest->abi_major = 1;
    manifest->abi_minor = 2;
    manifest->dependency_count = 1;
    strcpy(manifest->dependencies[0].name, "pci-core");
    strcpy(manifest->dependencies[0].minimum_version, "1.0");
    if (ctx->bad_name) strcpy(manifest->name, "../escape");
    if (ctx->bad_version) strcpy(manifest->version, "..");
    if (ctx->bad_dependency) strcpy(manifest->dependencies[0].name, "../escape");
    manifest->payload_offset = 4;
    manifest->payload_size = 8;
    return 0;
}

static int verify_signature(void *opaque, const void *package, size_t size)
{
    test_context_t *ctx = opaque;
    return package && size == 16 && ctx->signature_ok ? 0 : -1;
}

static int dependency_satisfied(void *opaque,
                                const struct kdrv_dependency *dependency)
{
    test_context_t *ctx = opaque;
    if (strcmp(dependency->name, "pci-core") != 0) return -1;
    return ctx->dependency_ok ? 0 : -1;
}

static int stage_payload(void *opaque, const char *name, const void *payload,
                         size_t payload_size, char *staging_path,
                         size_t staging_path_size)
{
    test_context_t *ctx = opaque;
    if (strcmp(name, "test-driver") != 0 || !payload || payload_size != 8)
        return -1;
    if (staging_path_size < sizeof "stage/test-driver") return -1;
    strcpy(staging_path, "stage/test-driver");
    ctx->staged = 1;
    return ctx->stage_fail ? -1 : 0;
}

static int commit_staged(void *opaque, const char *name,
                         const char *staging_path)
{
    test_context_t *ctx = opaque;
    if (strcmp(name, "test-driver") != 0 ||
        strcmp(staging_path, "stage/test-driver") != 0)
        return -1;
    if (ctx->commit_fail) return -1;
    ctx->committed = 1;
    return 0;
}

static void discard_staged(void *opaque, const char *staging_path)
{
    test_context_t *ctx = opaque;
    if (strcmp(staging_path, "stage/test-driver") != 0) abort();
    ctx->discarded++;
}

static int record_state(void *opaque, const char *name, const char *version,
                        enum kdrv_state state)
{
    test_context_t *ctx = opaque;
    if (strcmp(name, "test-driver") != 0 ||
        (state != KDRV_STATE_ACTIVE && strcmp(version, "1.2.3") != 0))
        return -1;
    if (ctx->state_count >= sizeof ctx->states / sizeof ctx->states[0])
        return -1;
    ctx->states[ctx->state_count++] = state;
    return 0;
}

static void setup(test_context_t *ctx, struct kdrv_manager_ops *ops)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->signature_ok = 1;
    ctx->dependency_ok = 1;
    memset(ops, 0, sizeof *ops);
    ops->context = ctx;
    ops->read_package = read_package;
    ops->free_package = free_package;
    ops->parse_manifest = parse_manifest;
    ops->verify_signature = verify_signature;
    ops->dependency_satisfied = dependency_satisfied;
    ops->stage_payload = stage_payload;
    ops->commit_staged = commit_staged;
    ops->discard_staged = discard_staged;
    ops->record_state = record_state;
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL %s:%d: %s\\n", __FILE__, __LINE__, #x); \
    return 1; \
} } while (0)

static int test_success(void)
{
    test_context_t ctx;
    struct kdrv_manager_ops ops;
    setup(&ctx, &ops);
    CHECK(kdrv_manager_install(&ops, "/drv/test.kdrv", "x86_64", 1, 2) == 0);
    CHECK(ctx.committed && !ctx.discarded && ctx.freed == 1);
    CHECK(ctx.state_count == 3);
    CHECK(ctx.states[0] == KDRV_STATE_VERIFIED);
    CHECK(ctx.states[1] == KDRV_STATE_STAGED);
    CHECK(ctx.states[2] == KDRV_STATE_INSTALLED);
    return 0;
}

static int test_failure_cleanup(int mode)
{
    test_context_t ctx;
    struct kdrv_manager_ops ops;
    setup(&ctx, &ops);
    if (mode == 0) ctx.signature_ok = 0;
    if (mode == 1) ctx.dependency_ok = 0;
    if (mode == 2) ctx.stage_fail = 1; /* backend reports failure after creating staging object */
    if (mode == 3) ctx.commit_fail = 1;
    CHECK(kdrv_manager_install(&ops, "/drv/test.kdrv", "x86_64", 1, 2) != 0);
    CHECK(ctx.freed == 1);
    CHECK(!ctx.committed);
    CHECK(ctx.state_count > 0);
    CHECK(ctx.states[ctx.state_count - 1] == KDRV_STATE_FAILED);
    if (mode == 2 || mode == 3) CHECK(ctx.discarded == 1);
    else CHECK(ctx.discarded == 0);
    return 0;
}

static int test_rejects_unsafe_metadata(int mode)
{
    test_context_t ctx;
    struct kdrv_manager_ops ops;
    setup(&ctx, &ops);
    if (mode == 0) ctx.bad_name = 1;
    if (mode == 1) ctx.bad_version = 1;
    if (mode == 2) ctx.bad_dependency = 1;
    CHECK(kdrv_manager_install(&ops, "/drv/test.kdrv", "x86_64", 1, 2) != 0);
    CHECK(ctx.freed == 1);
    CHECK(!ctx.staged && !ctx.committed && ctx.discarded == 0);
    CHECK(ctx.state_count == 0); /* metadata is rejected before trust/state changes */
    return 0;
}

int main(void)
{
    if (test_success()) return 1;
    for (int i = 0; i < 4; ++i)
        if (test_failure_cleanup(i)) return 1;
    for (int i = 0; i < 3; ++i)
        if (test_rejects_unsafe_metadata(i)) return 1;
    puts("kdrv manager lifecycle: PASS");
    return 0;
}
