#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RECORDS 300000u
#define BATCH 64u
#define CAPACITY 1024u
#define MASK (CAPACITY - 1u)
#define REPEATS 7u

typedef struct { uint64_t seq, a, b, check; } record_t;
typedef struct {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    record_t slots[CAPACITY];
} ring_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void fill_record(record_t *r, uint64_t i) {
    r->seq = i;
    r->a = i * 17u + 3u;
    r->b = i ^ 0x9e3779b97f4a7c15ull;
    r->check = r->seq ^ r->a ^ r->b ^ 0xd6e8feb86659fd93ull;
}
static int valid_record(const record_t *r, uint64_t i) {
    return r->seq == i && r->a == i * 17u + 3u &&
           r->b == (i ^ 0x9e3779b97f4a7c15ull) &&
           r->check == (r->seq ^ r->a ^ r->b ^ 0xd6e8feb86659fd93ull);
}
static int write_full(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int read_full(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        p += k; n -= (size_t)k;
    }
    return 0;
}
static int wait_child(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}
static int make_memfd(const char *name, size_t size) {
    int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size)) { close(fd); return -1; }
    return fd;
}
static double mib_s(uint64_t bytes, uint64_t ns) {
    return (double)bytes * 1e9 / (double)ns / 1048576.0;
}

/* Host analogue only: possession of a memfd permits mapping; without the FD it fails. */
static int descriptor_authority_test(void) {
    int fd = make_memfd("kira-capability-probe", 4096);
    if (fd < 0) return 0;
    unsigned char *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return 0; }
    pid_t allowed = fork();
    if (allowed == 0) {
        unsigned char *child = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (child == MAP_FAILED) _exit(10);
        child[0] = 0x5a;
        munmap(child, 4096);
        _exit(0);
    }
    int a = wait_child(allowed);
    int shared = map[0] == 0x5a;
    pid_t denied = fork();
    if (denied == 0) {
        munmap(map, 4096);
        close(fd);
        errno = 0;
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        _exit(p == MAP_FAILED && errno == EBADF ? 0 : 11);
    }
    int d = wait_child(denied);
    munmap(map, 4096);
    close(fd);
    return a == 0 && shared && d == 0;
}

/* Baseline: producer/consumer processes copy records through a kernel pipe. */
static int pipe_copy(uint64_t *elapsed) {
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[1]);
        record_t batch[BATCH];
        for (uint64_t base = 0; base < RECORDS; base += BATCH) {
            size_t n = RECORDS - base < BATCH ? RECORDS - (size_t)base : BATCH;
            if (read_full(fds[0], batch, n * sizeof(*batch))) _exit(20);
            for (size_t i = 0; i < n; ++i)
                if (!valid_record(&batch[i], base + i)) _exit(21);
        }
        close(fds[0]); _exit(0);
    }
    close(fds[0]);
    record_t batch[BATCH];
    uint64_t start = now_ns();
    for (uint64_t base = 0; base < RECORDS; base += BATCH) {
        size_t n = RECORDS - base < BATCH ? RECORDS - (size_t)base : BATCH;
        for (size_t i = 0; i < n; ++i) fill_record(&batch[i], base + i);
        if (write_full(fds[1], batch, n * sizeof(*batch))) { close(fds[1]); return -1; }
    }
    close(fds[1]);
    int rc = wait_child(pid);
    *elapsed = now_ns() - start;
    return rc;
}

/* Ring-copy: producer copies private record -> shared slot; consumer copies slot -> private record. */
static int shared_ring_copy(uint64_t *elapsed) {
    int fd = make_memfd("kira-ring-copy", sizeof(ring_t));
    if (fd < 0) return -1;
    ring_t *r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) { close(fd); return -1; }
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0); atomic_init(&r->tail, 0);
    if (!atomic_is_lock_free(&r->head) || !atomic_is_lock_free(&r->tail)) return -2;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        uint64_t tail = 0;
        for (uint64_t i = 0; i < RECORDS; ++i) {
            while (atomic_load_explicit(&r->head, memory_order_acquire) == tail) sched_yield();
            record_t local;
            memcpy(&local, &r->slots[tail & MASK], sizeof(local));
            if (!valid_record(&local, i)) _exit(30);
            ++tail;
            atomic_store_explicit(&r->tail, tail, memory_order_release);
        }
        _exit(0);
    }
    uint64_t head = 0, start = now_ns();
    for (uint64_t i = 0; i < RECORDS; ++i) {
        while (head - atomic_load_explicit(&r->tail, memory_order_acquire) == CAPACITY) sched_yield();
        record_t local; fill_record(&local, i);
        memcpy(&r->slots[head & MASK], &local, sizeof(local));
        ++head;
        atomic_store_explicit(&r->head, head, memory_order_release);
    }
    int rc = wait_child(pid);
    *elapsed = now_ns() - start;
    munmap(r, sizeof(*r)); close(fd);
    return rc;
}

/* Ring-zero-copy: producer writes into the shared slot; consumer validates it in place. */
static int shared_ring_zero_copy(uint64_t *elapsed) {
    int fd = make_memfd("kira-ring-zero-copy", sizeof(ring_t));
    if (fd < 0) return -1;
    ring_t *r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) { close(fd); return -1; }
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0); atomic_init(&r->tail, 0);
    if (!atomic_is_lock_free(&r->head) || !atomic_is_lock_free(&r->tail)) return -2;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        uint64_t tail = 0;
        for (uint64_t i = 0; i < RECORDS; ++i) {
            while (atomic_load_explicit(&r->head, memory_order_acquire) == tail) sched_yield();
            const record_t *shared = &r->slots[tail & MASK];
            if (!valid_record(shared, i)) _exit(40);
            ++tail;
            atomic_store_explicit(&r->tail, tail, memory_order_release);
        }
        _exit(0);
    }
    uint64_t head = 0, start = now_ns();
    for (uint64_t i = 0; i < RECORDS; ++i) {
        while (head - atomic_load_explicit(&r->tail, memory_order_acquire) == CAPACITY) sched_yield();
        fill_record(&r->slots[head & MASK], i);
        ++head;
        atomic_store_explicit(&r->head, head, memory_order_release);
    }
    int rc = wait_child(pid);
    *elapsed = now_ns() - start;
    munmap(r, sizeof(*r)); close(fd);
    return rc;
}
static void sort_u64(uint64_t *v, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        uint64_t x = v[i]; size_t j = i;
        while (j && v[j - 1] > x) { v[j] = v[j - 1]; --j; }
        v[j] = x;
    }
}
static int measure(const char *name, int (*fn)(uint64_t *), uint64_t out[REPEATS]) {
    for (unsigned i = 0; i < REPEATS; ++i) {
        if (fn(&out[i])) { fprintf(stderr, "%s: correctness failure on repetition %u\n", name, i + 1); return -1; }
    }
    return 0;
}
static void report(const char *name, uint64_t samples[REPEATS]) {
    uint64_t sorted[REPEATS]; memcpy(sorted, samples, sizeof(sorted)); sort_u64(sorted, REPEATS);
    uint64_t bytes = (uint64_t)RECORDS * sizeof(record_t);
    printf("%-22s median_ms=%.3f p95_ms=%.3f MiB_s=%.2f\n",
           name, sorted[REPEATS / 2] / 1e6, sorted[REPEATS - 1] / 1e6,
           mib_s(bytes, sorted[REPEATS / 2]));
}
int main(void) {
    uint64_t pipe_samples[REPEATS], copy_samples[REPEATS], zero_samples[REPEATS];
    if (!descriptor_authority_test()) { puts("IPC LAB: FAIL descriptor authority probe"); return 1; }
    if (measure("pipe-copy", pipe_copy, pipe_samples) ||
        measure("shared-ring-copy", shared_ring_copy, copy_samples) ||
        measure("shared-ring-zero-copy", shared_ring_zero_copy, zero_samples)) return 1;
    puts("descriptor_fd_present=ALLOW descriptor_fd_absent=DENY");
    puts("records_per_run=300000 repetitions=7 capacity=1024");
    report("pipe-copy", pipe_samples);
    report("shared-ring-copy", copy_samples);
    report("shared-ring-zero-copy", zero_samples);
    uint64_t p[REPEATS], z[REPEATS];
    memcpy(p, pipe_samples, sizeof(p)); memcpy(z, zero_samples, sizeof(z));
    sort_u64(p, REPEATS); sort_u64(z, REPEATS);
    printf("zero_copy_vs_pipe_speedup=%.3f\n", (double)p[REPEATS/2] / (double)z[REPEATS/2]);
    puts("correctness=PASS process_boundary=FORKED atomics=LOCK_FREE");
    puts("EVIDENCE=Linux host prototype; FD possession is not KIRA capability enforcement; not KIRA kernel IPC");
    return 0;
}
