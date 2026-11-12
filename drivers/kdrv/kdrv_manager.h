#ifndef KIRA_KDRV_MANAGER_H
#define KIRA_KDRV_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include "kdrv_trust.h"

#define KDRV_NAME_MAX 64u
#define KDRV_VERSION_MAX 32u
#define KDRV_DEPENDENCY_MAX 16u
#define KDRV_PACKAGE_MAX (32u * 1024u * 1024u)
#define KDRV_INSTALL_ROLLBACK_FAILED (-2)
#define KDRV_STATE_ROLLBACK_FAILED (-3)

enum kdrv_state {
    KDRV_STATE_ABSENT = 0,
    KDRV_STATE_STAGED,
    KDRV_STATE_VERIFIED,
    KDRV_STATE_INSTALLED,
    KDRV_STATE_ACTIVE,
    KDRV_STATE_DISABLED,
    KDRV_STATE_FAILED
};

struct kdrv_dependency {
    char name[KDRV_NAME_MAX];
    char minimum_version[KDRV_VERSION_MAX];
};

struct kdrv_manifest {
    char name[KDRV_NAME_MAX];
    char version[KDRV_VERSION_MAX];
    char target_arch[16];
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t dependency_count;
    struct kdrv_dependency dependencies[KDRV_DEPENDENCY_MAX];
    uint64_t payload_offset;
    uint64_t payload_size;
};

struct kdrv_manager_ops {
    void *context;
    int (*read_package)(void *context, const char *path,
                        void **data, size_t *size);
    void (*free_package)(void *context, void *data, size_t size);
    int (*parse_manifest)(void *context, const void *package, size_t size,
                          struct kdrv_manifest *manifest);
    int (*verify_signature)(void *context, const void *package, size_t size);
    int (*dependency_satisfied)(void *context,
                                const struct kdrv_dependency *dependency);
    int (*stage_payload)(void *context, const char *name,
                         const void *payload, size_t payload_size,
                         char *staging_path, size_t staging_path_size);
    int (*commit_staged)(void *context, const char *name,
                         const char *staging_path);
    /* Undo an already committed install if durable INSTALLED state cannot be recorded. */
    int (*rollback_committed)(void *context, const char *name);
    void (*discard_staged)(void *context, const char *staging_path);
    int (*activate)(void *context, const char *name);
    int (*deactivate)(void *context, const char *name);
    int (*record_state)(void *context, const char *name,
                        const char *version, enum kdrv_state state);
};

/* Returns KDRV_INSTALL_ROLLBACK_FAILED when a post-commit state write fails
   and the backend cannot remove the newly committed payload. */
int kdrv_manager_install(struct kdrv_manager_ops *ops, const char *path,
                         const char *target_arch,
                         uint32_t abi_major, uint32_t abi_minor);
int kdrv_manager_activate(struct kdrv_manager_ops *ops, const char *name);
int kdrv_manager_remove(struct kdrv_manager_ops *ops, const char *name);

#endif
