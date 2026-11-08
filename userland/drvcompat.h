#ifndef KIRA_DRVCOMPAT_H
#define KIRA_DRVCOMPAT_H
#include <stddef.h>
#include <stdint.h>
#define KIRA_DRVCOMPAT_NAME_MAX 64u
#define KIRA_DRVCOMPAT_HASH_SIZE 32u
#define KIRA_DRVCOMPAT_MAX_FILES 32u
#define KIRA_DRVCOMPAT_MAX_FILE_SIZE (8u*1024u*1024u)
#define KIRA_DRVCOMPAT_MAX_TOTAL_SIZE (16u*1024u*1024u)
#define KIRA_DRVCOMPAT_MAX_ARCHIVE_SIZE (16u*1024u*1024u)
#define KIRA_DRVCOMPAT_MAX_OUTPUT_SIZE (8u*1024u*1024u)
enum kira_drvcompat_stage {
 KIRA_DRV_FETCHED=1,KIRA_DRV_AUTHENTICATED,KIRA_DRV_INSPECTED,
 KIRA_DRV_TRANSLATED,KIRA_DRV_BUILT,KIRA_DRV_SIGNED,KIRA_DRV_PUBLISHED
};
struct kira_drvcompat_file { char path[192]; const uint8_t *data; size_t size; uint8_t digest[32]; };
struct kira_drvcompat_manifest {
 char name[KIRA_DRVCOMPAT_NAME_MAX]; char source_os[24]; char target_arch[16];
 uint32_t source_abi,target_abi; size_t file_count;
 struct kira_drvcompat_file files[KIRA_DRVCOMPAT_MAX_FILES];
};
/*
 * Ownership contract:
 * - fetch_archive returns one buffer released by release_archive.
 * - unpack_safe and translate_driver must keep manifest file data alive until
 *   kira_drvcompat_convert returns (normally backed by archive or adapter-owned
 *   scratch storage).
 * - build_native/sign_native return buffers released by release_buffer.
 * - publish_package must finish copying/committing the package before returning.
 * All fetch adapters must enforce their own streaming size limit as well.
 */
struct kira_drvcompat_ops {
 void *context;
 int (*fetch_archive)(void *,const char *uri,void **archive,size_t *size);
 void (*release_archive)(void *,void *archive,size_t size);
 int (*verify_publisher)(void *,const void *archive,size_t size);
 int (*unpack_safe)(void *,const void *archive,size_t size,
                    struct kira_drvcompat_manifest *manifest);
 int (*hash)(void *,const void *,size_t,uint8_t digest[32]);
 int (*inspect_source)(void *,const struct kira_drvcompat_manifest *);
 int (*translate_driver)(void *,const struct kira_drvcompat_manifest *,
                         struct kira_drvcompat_manifest *translated);
 int (*build_native)(void *,const struct kira_drvcompat_manifest *,
                     void **binary,size_t *binary_size);
 int (*sign_native)(void *,const char *name,const void *binary,size_t size,
                    void **signed_package,size_t *signed_size);
 int (*publish_package)(void *,const char *name,const void *package,size_t size);
 void (*release_buffer)(void *,void *buffer,size_t size);
 void (*progress)(void *,enum kira_drvcompat_stage,int status);
};
int kira_drvcompat_convert(const struct kira_drvcompat_ops *ops,
                           const char *source_uri);
#endif
