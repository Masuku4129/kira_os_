/*
 * KIRA driver compatibility conversion pipeline.
 *
 * This is a bounded, fail-closed orchestration layer. It never executes
 * foreign driver binaries and publishes only a locally built and signed
 * target-ABI package. Platform adapters still own archive extraction,
 * source analysis, ABI translation, compilation and cryptographic signing.
 */
#include "drvcompat.h"

static int str_ok(const char *s, size_t cap)
{
    size_t i;
    if (!s || !cap)
        return 0;
    for (i = 0; i < cap; ++i)
        if (!s[i])
            return i != 0;
    return 0;
}

static int token_ok(const char *s, size_t cap, int lower_only)
{
    size_t i;
    if (!str_ok(s, cap))
        return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') ||
              (!lower_only && c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int same_string(const char *a, const char *b)
{
    size_t i = 0;
    if (!a || !b)
        return 0;
    while (a[i] && b[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}

static int path_safe(const char *p)
{
    size_t i = 0, component = 0;
    if (!str_ok(p, 192) || p[0] == '/' || p[0] == '\\')
        return 0;
    while (p[i]) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c > 0x7e || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || c == '\\')
            return 0;
        if (c == '/') {
            if (!component ||
                (component == 1 && p[i - 1] == '.') ||
                (component == 2 && p[i - 1] == '.' && p[i - 2] == '.'))
                return 0;
            component = 0;
        } else {
            ++component;
        }
        ++i;
    }
    if (!component ||
        (component == 1 && p[i - 1] == '.') ||
        (component == 2 && p[i - 1] == '.' && p[i - 2] == '.'))
        return 0;
    return 1;
}

static void report(const struct kira_drvcompat_ops *o,
                   enum kira_drvcompat_stage stage, int status)
{
    if (o->progress)
        o->progress(o->context, stage, status);
}

static int validate_manifest(const struct kira_drvcompat_manifest *m)
{
    size_t i, j, total = 0;
    if (!m || !token_ok(m->name, sizeof(m->name), 1) ||
        !token_ok(m->source_os, sizeof(m->source_os), 0) ||
        !token_ok(m->target_arch, sizeof(m->target_arch), 0) ||
        !m->source_abi || !m->target_abi ||
        !m->file_count || m->file_count > KIRA_DRVCOMPAT_MAX_FILES)
        return -1;

    for (i = 0; i < m->file_count; ++i) {
        const struct kira_drvcompat_file *f = &m->files[i];
        if (!path_safe(f->path) || !f->data || !f->size ||
            f->size > KIRA_DRVCOMPAT_MAX_FILE_SIZE ||
            f->size > KIRA_DRVCOMPAT_MAX_TOTAL_SIZE - total)
            return -2;
        total += f->size;
        for (j = 0; j < i; ++j)
            if (same_string(f->path, m->files[j].path))
                return -3;
    }
    return 0;
}

static int verify_files(const struct kira_drvcompat_ops *o,
                        const struct kira_drvcompat_manifest *m)
{
    size_t i;
    uint8_t digest[32];
    for (i = 0; i < m->file_count; ++i) {
        unsigned j;
        uint8_t diff = 0;
        if (o->hash(o->context, m->files[i].data,
                    m->files[i].size, digest) != 0)
            return -1;
        for (j = 0; j < sizeof(digest); ++j)
            diff |= (uint8_t)(digest[j] ^ m->files[i].digest[j]);
        if (diff)
            return -2;
    }
    return 0;
}

int kira_drvcompat_convert(const struct kira_drvcompat_ops *o,
                           const char *uri,
                           const char *target_arch, uint32_t target_abi)
{
    void *archive = 0, *binary = 0, *package = 0;
    size_t archive_size = 0, binary_size = 0, package_size = 0;
    struct kira_drvcompat_manifest source = { 0 }, native = { 0 };
    int native_started = 0, rc = -1;

    if (!o || !str_ok(uri, 1024) ||
        !token_ok(target_arch, 16, 0) || !target_abi ||
        !o->fetch_archive || !o->release_archive ||
        !o->verify_publisher || !o->unpack_safe || !o->hash ||
        !o->inspect_source || !o->translate_driver || !o->build_native ||
        !o->sign_native || !o->publish_package || !o->release_buffer ||
        !o->release_manifest)
        return -1;

    rc = -2; /* fetch */
    if (o->fetch_archive(o->context, uri, &archive, &archive_size) != 0 ||
        !archive || !archive_size ||
        archive_size > KIRA_DRVCOMPAT_MAX_ARCHIVE_SIZE)
        goto done;
    report(o, KIRA_DRV_FETCHED, 0);

    rc = -3; /* publisher authentication */
    if (o->verify_publisher(o->context, archive, archive_size) != 0)
        goto done;
    report(o, KIRA_DRV_AUTHENTICATED, 0);

    rc = -4; /* safe extraction and source manifest */
    if (o->unpack_safe(o->context, archive, archive_size, &source) != 0 ||
        validate_manifest(&source) != 0 ||
        verify_files(o, &source) != 0)
        goto done;

    rc = -5; /* source ABI / source semantics */
    if (o->inspect_source(o->context, &source) != 0)
        goto done;
    report(o, KIRA_DRV_INSPECTED, 0);

    rc = -6; /* target translation */
    native_started = 1; /* release partial translator output on every exit */
    if (o->translate_driver(o->context, &source, &native) != 0 ||
        validate_manifest(&native) != 0 ||
        verify_files(o, &native) != 0 ||
        !same_string(native.target_arch, target_arch) ||
        native.target_abi != target_abi)
        goto done;
    report(o, KIRA_DRV_TRANSLATED, 0);

    rc = -7; /* native build */
    if (o->build_native(o->context, &native, &binary, &binary_size) != 0 ||
        !binary || !binary_size ||
        binary_size > KIRA_DRVCOMPAT_MAX_OUTPUT_SIZE)
        goto done;
    report(o, KIRA_DRV_BUILT, 0);

    rc = -8; /* local package signing */
    if (o->sign_native(o->context, native.name, binary, binary_size,
                       &package, &package_size) != 0 ||
        !package || !package_size ||
        package_size > KIRA_DRVCOMPAT_MAX_OUTPUT_SIZE)
        goto done;
    report(o, KIRA_DRV_SIGNED, 0);

    rc = -9; /* publication */
    if (o->publish_package(o->context, native.name,
                           package, package_size) != 0)
        goto done;
    report(o, KIRA_DRV_PUBLISHED, 0);
    rc = 0;

done:
    if (package)
        o->release_buffer(o->context, package, package_size);
    if (binary)
        o->release_buffer(o->context, binary, binary_size);
    if (native_started)
        o->release_manifest(o->context, &native);
    if (archive)
        o->release_archive(o->context, archive, archive_size);
    if (rc)
        report(o, KIRA_DRV_FAILED, rc);
    return rc;
}
