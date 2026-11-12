#include "../userland/drvcompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "drvcompat_test: FAIL line %d: %s\n", __LINE__, #x); \
    return 1; \
} } while (0)

enum {
    FAIL_NONE, FAIL_VERIFY, FAIL_INSPECT, FAIL_TRANSLATE,
    FAIL_BUILD, FAIL_SIGN, FAIL_PUBLISH
};

typedef struct {
    int fail_stage;
    int bad_path, duplicate_path, bad_digest, wrong_arch, wrong_abi;
    int oversized_archive, oversized_total;
    int published, archive_releases, manifest_releases, buffer_releases;
    int progress_count;
    enum kira_drvcompat_stage progress_stage[16];
    int progress_status[16];
    uint8_t source_bytes[4];
    uint8_t *native_bytes;
} test_ctx_t;

static int fetch_archive(void *opaque, const char *uri, void **archive, size_t *size)
{
    test_ctx_t *ctx = opaque;
    uint8_t *p;
    if (strcmp(uri, "https://example.org/netdrv.kira-src") != 0)
        return -1;
    p = malloc(16);
    if (!p) return -1;
    memset(p, 0x5a, 16);
    *archive = p;
    *size = ctx->oversized_archive ? KIRA_DRVCOMPAT_MAX_ARCHIVE_SIZE + 1u : 16u;
    return 0;
}

static void release_archive(void *opaque, void *archive, size_t size)
{
    test_ctx_t *ctx = opaque;
    (void)size;
    ctx->archive_releases++;
    free(archive);
}

static int verify_publisher(void *opaque, const void *archive, size_t size)
{
    test_ctx_t *ctx = opaque;
    return archive && size && ctx->fail_stage != FAIL_VERIFY ? 0 : -1;
}

static int test_hash(void *opaque, const void *data, size_t size, uint8_t digest[32])
{
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    size_t i;
    (void)opaque;
    if (!data || !size) return -1;
    for (i = 0; i < size; ++i)
        h = (h ^ p[i]) * 16777619u;
    for (i = 0; i < 32; ++i) {
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        digest[i] = (uint8_t)(h >> ((i & 3u) * 8u));
    }
    return 0;
}

static int unpack_safe(void *opaque, const void *archive, size_t size,
                       struct kira_drvcompat_manifest *m)
{
    test_ctx_t *ctx = opaque;
    size_t i;
    if (!archive || size < 16) return -1;
    memset(m, 0, sizeof(*m));
    strcpy(m->name, "netdrv");
    strcpy(m->source_os, "linux");
    strcpy(m->target_arch, "x86_64");
    m->source_abi = 1;
    m->target_abi = 1;
    m->file_count = ctx->oversized_total ? 5 : (ctx->duplicate_path ? 2 : 1);
    for (i = 0; i < m->file_count; ++i) {
        struct kira_drvcompat_file *f = &m->files[i];
        if (ctx->bad_path)
            strcpy(f->path, "../escape.c");
        else if (ctx->duplicate_path)
            strcpy(f->path, "src/netdrv.c");
        else {
            strcpy(f->path, "src/netdrv.c");
        }
        f->data = ctx->source_bytes;
        f->size = ctx->oversized_total ? KIRA_DRVCOMPAT_MAX_FILE_SIZE : sizeof ctx->source_bytes;
        if (test_hash(ctx, f->data, sizeof ctx->source_bytes, f->digest) != 0)
            return -1;
        if (ctx->bad_digest) f->digest[0] ^= 1;
    }
    return 0;
}

static int inspect_source(void *opaque, const struct kira_drvcompat_manifest *m)
{
    test_ctx_t *ctx = opaque;
    return ctx->fail_stage == FAIL_INSPECT || strcmp(m->source_os, "linux") ||
           m->source_abi != 1 ? -1 : 0;
}

static int translate_driver(void *opaque, const struct kira_drvcompat_manifest *src,
                            struct kira_drvcompat_manifest *dst)
{
    test_ctx_t *ctx = opaque;
    if (ctx->fail_stage == FAIL_TRANSLATE) return -1;
    *dst = *src;
    strcpy(dst->target_arch, ctx->wrong_arch ? "aarch64" : "x86_64");
    dst->target_abi = ctx->wrong_abi ? 3u : 2u;
    ctx->native_bytes = malloc(sizeof ctx->source_bytes);
    if (!ctx->native_bytes) return -1;
    memcpy(ctx->native_bytes, ctx->source_bytes, sizeof ctx->source_bytes);
    dst->files[0].data = ctx->native_bytes;
    dst->files[0].size = sizeof ctx->source_bytes;
    if (test_hash(ctx, dst->files[0].data, dst->files[0].size,
                  dst->files[0].digest) != 0)
        return -1;
    return 0;
}

static void release_manifest(void *opaque, struct kira_drvcompat_manifest *m)
{
    test_ctx_t *ctx = opaque;
    (void)m;
    ctx->manifest_releases++;
    free(ctx->native_bytes);
    ctx->native_bytes = 0;
}

static int build_native(void *opaque, const struct kira_drvcompat_manifest *m,
                        void **binary, size_t *size)
{
    test_ctx_t *ctx = opaque;
    uint8_t *p;
    if (ctx->fail_stage == FAIL_BUILD) return -1;
    p = malloc(8);
    if (!p) return -1;
    memset(p, 0x42, 8);
    *binary = p;
    *size = 8;
    return m->target_abi == 2 ? 0 : -1;
}

static int sign_native(void *opaque, const char *name, const void *binary,
                       size_t size, void **package, size_t *package_size)
{
    test_ctx_t *ctx = opaque;
    uint8_t *p;
    if (ctx->fail_stage == FAIL_SIGN || strcmp(name, "netdrv") ||
        !binary || !size) return -1;
    p = malloc(12);
    if (!p) return -1;
    memset(p, 0x7c, 12);
    *package = p;
    *package_size = 12;
    return 0;
}

static int publish_package(void *opaque, const char *name,
                           const void *package, size_t size)
{
    test_ctx_t *ctx = opaque;
    if (ctx->fail_stage == FAIL_PUBLISH || strcmp(name, "netdrv") ||
        !package || size != 12) return -1;
    ctx->published++;
    return 0;
}

static void release_buffer(void *opaque, void *buffer, size_t size)
{
    test_ctx_t *ctx = opaque;
    (void)size;
    ctx->buffer_releases++;
    free(buffer);
}

static void progress(void *opaque, enum kira_drvcompat_stage stage, int status)
{
    test_ctx_t *ctx = opaque;
    if (ctx->progress_count < 16) {
        ctx->progress_stage[ctx->progress_count] = stage;
        ctx->progress_status[ctx->progress_count] = status;
        ctx->progress_count++;
    }
}

static void setup(test_ctx_t *ctx, struct kira_drvcompat_ops *ops)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->source_bytes[0] = 1;
    ctx->source_bytes[1] = 2;
    ctx->source_bytes[2] = 3;
    ctx->source_bytes[3] = 4;
    memset(ops, 0, sizeof(*ops));
    ops->context = ctx;
    ops->fetch_archive = fetch_archive;
    ops->release_archive = release_archive;
    ops->verify_publisher = verify_publisher;
    ops->unpack_safe = unpack_safe;
    ops->hash = test_hash;
    ops->inspect_source = inspect_source;
    ops->translate_driver = translate_driver;
    ops->build_native = build_native;
    ops->sign_native = sign_native;
    ops->publish_package = publish_package;
    ops->release_buffer = release_buffer;
    ops->release_manifest = release_manifest;
    ops->progress = progress;
}

static int run_failure_case(int mode)
{
    test_ctx_t ctx;
    struct kira_drvcompat_ops ops;
    int rc;
    setup(&ctx, &ops);
    switch (mode) {
    case 0: ctx.fail_stage = FAIL_VERIFY; break;
    case 1: ctx.bad_path = 1; break;
    case 2: ctx.duplicate_path = 1; break;
    case 3: ctx.bad_digest = 1; break;
    case 4: ctx.wrong_arch = 1; break;
    case 5: ctx.wrong_abi = 1; break;
    case 6: ctx.fail_stage = FAIL_BUILD; break;
    case 7: ctx.fail_stage = FAIL_SIGN; break;
    case 8: ctx.fail_stage = FAIL_PUBLISH; break;
    case 9: ctx.oversized_archive = 1; break;
    case 10: ctx.oversized_total = 1; break;
    default: return 1;
    }
    rc = kira_drvcompat_convert(&ops, "https://example.org/netdrv.kira-src",
                                "x86_64", 2);
    CHECK(rc != 0);
    CHECK(ctx.published == 0);
    CHECK(ctx.archive_releases == 1);
    CHECK(ctx.progress_count > 0);
    CHECK(ctx.progress_stage[ctx.progress_count - 1] == KIRA_DRV_FAILED);
    if (mode >= 4 && mode <= 8)
        CHECK(ctx.manifest_releases == 1);
    else
        CHECK(ctx.manifest_releases == 0);
    if (mode == 6 || mode == 7 || mode == 8)
        CHECK(ctx.buffer_releases >= 1);
    return 0;
}

int main(void)
{
    test_ctx_t ctx;
    struct kira_drvcompat_ops ops;
    setup(&ctx, &ops);
    CHECK(kira_drvcompat_convert(&ops, "https://example.org/netdrv.kira-src",
                                 "x86_64", 2) == 0);
    CHECK(ctx.published == 1 && ctx.archive_releases == 1);
    CHECK(ctx.manifest_releases == 1 && ctx.buffer_releases == 2);
    CHECK(ctx.progress_count == 6);
    CHECK(ctx.progress_stage[0] == KIRA_DRV_FETCHED);
    CHECK(ctx.progress_stage[5] == KIRA_DRV_PUBLISHED);
    for (int i = 0; i < 11; ++i)
        CHECK(run_failure_case(i) == 0);
    puts("drvcompat: bounded extraction, metadata, ABI, digest, cleanup and signed publish PASS");
    return 0;
}
