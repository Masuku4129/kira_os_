/*
 * KDRV package lifecycle coordinator.
 *
 * The manager enforces a strict order: bounded package read, manifest
 * validation, signature verification, architecture/ABI checks, dependency
 * checks, staging, atomic commit, then durable state transition.
 * Filesystem atomicity and module loading are explicit platform callbacks.
 */

#include "kdrv_manager.h"

static int bounded_string(const char *s, size_t capacity)
{
    size_t i;
    if (!s || !capacity)
        return 0;
    for (i = 0; i < capacity; ++i)
        if (s[i] == '\0')
            return i != 0;
    return 0;
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

static int package_name_safe(const char *s, size_t capacity)
{
    size_t i;
    if (!bounded_string(s, capacity))
        return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int package_version_safe(const char *s, size_t capacity)
{
    size_t i;
    if (!bounded_string(s, capacity))
        return 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '_' || c == '+'))
            return 0;
    }
    return 1;
}

static int manifest_valid(const struct kdrv_manifest *m,
                          size_t package_size)
{
    uint32_t i;

    if (!m || !package_name_safe(m->name, sizeof(m->name)) ||
        !package_version_safe(m->version, sizeof(m->version)) ||
        !bounded_string(m->target_arch, sizeof(m->target_arch)) ||
        m->dependency_count > KDRV_DEPENDENCY_MAX ||
        m->payload_size == 0 ||
        m->payload_size > KDRV_PACKAGE_MAX ||
        m->payload_offset > package_size ||
        m->payload_size > package_size - m->payload_offset)
        return 0;

    for (i = 0; i < m->dependency_count; ++i)
        if (!package_name_safe(m->dependencies[i].name,
                               sizeof(m->dependencies[i].name)) ||
            !package_version_safe(m->dependencies[i].minimum_version,
                                  sizeof(m->dependencies[i].minimum_version)))
            return 0;

    return 1;
}

int kdrv_manager_install(struct kdrv_manager_ops *ops, const char *path,
                         const char *target_arch,
                         uint32_t abi_major, uint32_t abi_minor)
{
    void *package = 0;
    size_t package_size = 0;
    struct kdrv_manifest manifest;
    char staging_path[256];
    uint32_t i;
    int rc = -1;
    int staged = 0;
    int manifest_ready = 0;

    if (!ops || !path || !target_arch || !ops->read_package ||
        !ops->free_package || !ops->parse_manifest ||
        !ops->verify_signature || !ops->dependency_satisfied ||
        !ops->stage_payload || !ops->commit_staged ||
        !ops->discard_staged || !ops->record_state)
        return -1;

    staging_path[0] = '\0';

    if (ops->read_package(ops->context, path, &package, &package_size) != 0 ||
        !package || package_size == 0 || package_size > KDRV_PACKAGE_MAX)
        goto out;

    if (ops->parse_manifest(ops->context, package, package_size,
                            &manifest) != 0 ||
        !manifest_valid(&manifest, package_size))
        goto out;
    manifest_ready = 1;

    /* No package content is trusted before the signature callback succeeds. */
    if (ops->verify_signature(ops->context, package, package_size) != 0)
        goto out;

    if (!same_string(manifest.target_arch, target_arch) ||
        manifest.abi_major != abi_major ||
        manifest.abi_minor > abi_minor)
        goto out;

    for (i = 0; i < manifest.dependency_count; ++i)
        if (ops->dependency_satisfied(ops->context,
                                      &manifest.dependencies[i]) != 0)
            goto out;

    if (ops->record_state(ops->context, manifest.name, manifest.version,
                          KDRV_STATE_VERIFIED) != 0)
        goto out;

    {
        int stage_rc = ops->stage_payload(ops->context, manifest.name,
            (const unsigned char *)package + manifest.payload_offset,
            (size_t)manifest.payload_size,
            staging_path, sizeof(staging_path));
        /* A backend may have created a temporary object before reporting an
           error. If it returned a bounded path, always attempt to discard it. */
        staged = bounded_string(staging_path, sizeof(staging_path));
        if (stage_rc != 0 || !staged)
            goto out;
    }

    if (ops->record_state(ops->context, manifest.name, manifest.version,
                          KDRV_STATE_STAGED) != 0)
        goto out;

    if (ops->commit_staged(ops->context, manifest.name, staging_path) != 0)
        goto out;

    staged = 0;
    if (ops->record_state(ops->context, manifest.name, manifest.version,
                          KDRV_STATE_INSTALLED) != 0)
        goto out;

    rc = 0;

out:
    if (staged)
        ops->discard_staged(ops->context, staging_path);
    if (rc != 0 && manifest_ready)
        (void)ops->record_state(ops->context, manifest.name, manifest.version,
                                KDRV_STATE_FAILED);
    if (package)
        ops->free_package(ops->context, package, package_size);
    return rc;
}

int kdrv_manager_activate(struct kdrv_manager_ops *ops, const char *name)
{
    if (!ops || !name || !ops->activate || !ops->record_state)
        return -1;
    if (ops->activate(ops->context, name) != 0)
        return -1;
    return ops->record_state(ops->context, name, "", KDRV_STATE_ACTIVE);
}

int kdrv_manager_remove(struct kdrv_manager_ops *ops, const char *name)
{
    if (!ops || !name || !ops->deactivate || !ops->record_state)
        return -1;
    if (ops->deactivate(ops->context, name) != 0)
        return -1;
    return ops->record_state(ops->context, name, "", KDRV_STATE_ABSENT);
}
