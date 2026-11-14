#include "kcc.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    volatile int entered;
    volatile int release;
    int rc;
    KResult result;
    void *arena;
} Shared;

static const char good_src[] = "int value = 7; int f(void) { return value; }\n";
static const char bad_src[] = "int = ;\n";

static const char* plain_read(void* ud, const char* path, size_t* len) {
    (void)ud; (void)path;
    *len = sizeof good_src - 1;
    return good_src;
}
static const char* bad_read(void* ud, const char* path, size_t* len) {
    (void)ud; (void)path;
    *len = sizeof bad_src - 1;
    return bad_src;
}
static const char* blocking_read(void* ud, const char* path, size_t* len) {
    Shared* s = (Shared*)ud;
    (void)path;
    __sync_lock_test_and_set(&s->entered, 1);
    while (!__sync_fetch_and_add(&s->release, 0)) sched_yield();
    *len = sizeof good_src - 1;
    return good_src;
}
static void* compile_outer(void* ud) {
    Shared* s = (Shared*)ud;
    KCfg cfg = { blocking_read, s, 0, 0, 0, 0, "x86_64", 0 };
    s->rc = kcc_compile(&cfg, s->arena, 4u << 20, "outer.c", 0, &s->result);
    return 0;
}
int main(void) {
    Shared s = {0};
    s.arena = malloc(4u << 20);
    void* second_arena = malloc(4u << 20);
    void* retry_arena = malloc(4u << 20);
    if (!s.arena || !second_arena || !retry_arena) return 2;

    pthread_t thread;
    if (pthread_create(&thread, 0, compile_outer, &s)) return 2;
    while (!__sync_fetch_and_add(&s.entered, 0)) sched_yield();

    KCfg second_cfg = { plain_read, 0, 0, 0, 0, 0, "x86_64", 0 };
    KResult second_result;
    int second_rc = kcc_compile(&second_cfg, second_arena, 4u << 20, "second.c", 0, &second_result);
    if (second_rc != KCC_BUSY || !second_result.err || !strstr(second_result.err, "active compilation")) {
        fprintf(stderr, "parallel call was not rejected safely: rc=%d err=%s\n", second_rc,
                second_result.err ? second_result.err : "(null)");
        __sync_lock_test_and_set(&s.release, 1);
        pthread_join(thread, 0);
        return 1;
    }

    __sync_lock_test_and_set(&s.release, 1);
    pthread_join(thread, 0);
    if (s.rc != KCC_OK) {
        fprintf(stderr, "primary compilation failed: rc=%d err=%s\n", s.rc,
                s.result.err ? s.result.err : "(null)");
        return 1;
    }

    KCfg bad_cfg = { bad_read, 0, 0, 0, 0, 0, "x86_64", 0 };
    KResult bad_result;
    if (kcc_compile(&bad_cfg, second_arena, 4u << 20, "bad.c", 0, &bad_result) != KCC_ERROR) {
        fprintf(stderr, "invalid source was not reported as a compile error\n");
        return 1;
    }
    KResult retry_result;
    if (kcc_compile(&second_cfg, retry_arena, 4u << 20, "retry.c", 0, &retry_result) != KCC_OK) {
        fprintf(stderr, "compiler guard was not released after error: %s\n",
                retry_result.err ? retry_result.err : "(null)");
        return 1;
    }
    puts("kcc: concurrent entry rejected safely; lock released after success and error");
    free(s.arena); free(second_arena); free(retry_arena);
    return 0;
}
