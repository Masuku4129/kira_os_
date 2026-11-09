#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int timed_lock(pthread_mutex_t *m, int seconds) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return errno;
    ts.tv_sec += seconds;
    return pthread_mutex_timedlock(m, &ts);
}
static int init_mutex(pthread_mutex_t *m, int shared) {
    pthread_mutexattr_t a;
    int e = pthread_mutexattr_init(&a);
    if (e) return e;
    e = pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    if (!e && shared) e = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (!e) e = pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return e;
}
static pthread_mutex_t thread_mutex;
static void *die_holding_mutex(void *unused) {
    (void)unused;
    int e = pthread_mutex_lock(&thread_mutex);
    if (e) return (void *)(intptr_t)e;
    pthread_exit(NULL);
}
int main(void) {
    int passed = 0, total = 2;
    printf("KIRA robust-futex native runner diagnostic\n");
    printf("arch=%s pid=%ld\n",
#if defined(__aarch64__)
           "aarch64",
#elif defined(__x86_64__)
           "x86_64",
#else
           "other",
#endif
           (long)getpid());
#ifdef SYS_get_robust_list
    errno = 0;
    struct robust_list_head *head = NULL;
    size_t head_len = 0;
    long gr = syscall(SYS_get_robust_list, 0, &head, &head_len);
    printf("DIAG | get_robust_list=%s errno=%d (%s)\n",
           gr == 0 ? "available" : "unavailable", gr == 0 ? 0 : errno,
           gr == 0 ? "success" : strerror(errno));
#else
    printf("DIAG | SYS_get_robust_list not defined by headers\n");
#endif

    int e = init_mutex(&thread_mutex, 0);
    if (e) {
        printf("FAIL | thread robust mutex init: %s (%d)\n", strerror(e), e);
    } else {
        pthread_t t;
        e = pthread_create(&t, NULL, die_holding_mutex, NULL);
        if (!e) {
            void *ret = NULL;
            pthread_join(t, &ret);
            if (ret != NULL) {
                printf("FAIL | owner thread lock: %s\n", strerror((int)(intptr_t)ret));
            } else {
                e = timed_lock(&thread_mutex, 3);
                if (e == EOWNERDEAD) {
                    printf("PASS | thread owner-death recovery: EOWNERDEAD\n");
                    int c = pthread_mutex_consistent(&thread_mutex);
                    int u = c ? c : pthread_mutex_unlock(&thread_mutex);
                    if (u) printf("FAIL | thread mutex repair/unlock: %s (%d)\n", strerror(u), u);
                    else passed++;
                } else printf("FAIL | thread owner-death recovery: %s (%d)\n", strerror(e), e);
            }
            pthread_mutex_destroy(&thread_mutex);
        } else printf("FAIL | pthread_create: %s (%d)\n", strerror(e), e);
    }

    pthread_mutex_t *pm = mmap(NULL, sizeof(*pm), PROT_READ | PROT_WRITE,
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (pm == MAP_FAILED) {
        printf("FAIL | process shared mmap: %s\n", strerror(errno));
    } else {
        e = init_mutex(pm, 1);
        if (e) printf("FAIL | process-shared robust mutex init: %s (%d)\n", strerror(e), e);
        else {
            pid_t p = fork();
            if (p == 0) {
                int ce = timed_lock(pm, 3);
                if (ce) _exit(20);
                _exit(0);
            } else if (p < 0) printf("FAIL | fork: %s\n", strerror(errno));
            else {
                int status = 0;
                if (waitpid(p, &status, 0) < 0) printf("FAIL | waitpid: %s\n", strerror(errno));
                else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                    printf("FAIL | child did not acquire mutex (status=0x%x)\n", status);
                else {
                    e = timed_lock(pm, 3);
                    if (e == EOWNERDEAD) {
                        printf("PASS | process owner-death recovery: EOWNERDEAD\n");
                        int c = pthread_mutex_consistent(pm);
                        int u = c ? c : pthread_mutex_unlock(pm);
                        if (u) printf("FAIL | process mutex repair/unlock: %s (%d)\n", strerror(u), u);
                        else passed++;
                    } else printf("FAIL | process owner-death recovery: %s (%d)\n", strerror(e), e);
                }
            }
            pthread_mutex_destroy(pm);
        }
        munmap(pm, sizeof(*pm));
    }
    printf("SUMMARY | %d/%d recovery checks passed\n", passed, total);
    return passed == total ? 0 : 1;
}