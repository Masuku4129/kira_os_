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

typedef struct { uint64_t seq, a, b, check; } record_t;
typedef struct { _Atomic uint64_t head, tail; record_t slots[CAPACITY]; } ring_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { perror("clock_gettime"); exit(2); }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static record_t make_record(uint64_t i) {
    record_t r = {i, i * 17u + 3u, i ^ 0x9e3779b97f4a7c15ull, 0};
    r.check = r.seq ^ r.a ^ r.b ^ 0xd6e8feb86659fd93ull;
    return r;
}
static int valid(const record_t *r, uint64_t expected) {
    return r->seq == expected && r->check == (r->seq ^ r->a ^ r->b ^ 0xd6e8feb86659fd93ull);
}
static int write_full(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) { ssize_t k = write(fd, p, n); if (k < 0 && errno == EINTR) continue; if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}
static int read_full(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) { ssize_t k = read(fd, p, n); if (k < 0 && errno == EINTR) continue; if (k <= 0) return -1; p += k; n -= (size_t)k; }
    return 0;
}
static double mib_s(uint64_t bytes, uint64_t ns) {
    return (double)bytes * 1000000000.0 / (double)ns / 1048576.0;
}
static int wait_child(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

/* Baseline: separate producer/consumer processes exchange copied records via a pipe. */
static int pipe_test(uint64_t *elapsed) {
    int fds[2];
    if (pipe(fds)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fds[1]);
        record_t batch[BATCH];
        for (uint64_t base = 0; base < RECORDS; base += BATCH) {
            size_t n = RECORDS - base < BATCH ? (size_t)(RECORDS - base) : BATCH;
            if (read_full(fds[0], batch, n * sizeof(record_t))) _exit(10);
            for (size_t i = 0; i < n; ++i) if (!valid(&batch[i], base + i)) _exit(11);
        }
        close(fds[0]); _exit(0);
    }
    close(fds[0]);
    record_t batch[BATCH];
    uint64_t start = now_ns();
    for (uint64_t base = 0; base < RECORDS; base += BATCH) {
        size_t n = RECORDS - base < BATCH ? (size_t)(RECORDS - base) : BATCH;
        for (size_t i = 0; i < n; ++i) batch[i] = make_record(base + i);
        if (write_full(fds[1], batch, n * sizeof(record_t))) { close(fds[1]); return -1; }
    }
    close(fds[1]);
    int rc = wait_child(pid);
    uint64_t end = now_ns();
    *elapsed = end - start;
    return rc;
}

/* Candidate: one producer and one consumer in separate processes over MAP_SHARED memory. */
static int shared_ring_test(uint64_t *elapsed) {
    ring_t *r = mmap(NULL, sizeof(*r), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED) return -1;
    memset(r, 0, sizeof(*r));
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    if (!atomic_is_lock_free(&r->head) || !atomic_is_lock_free(&r->tail)) { munmap(r, sizeof(*r)); return -2; }
    pid_t pid = fork();
    if (pid < 0) { munmap(r, sizeof(*r)); return -1; }
    if (pid == 0) {
        uint64_t tail = 0;
        for (uint64_t i = 0; i < RECORDS; ++i) {
            uint64_t head;
            while ((head = atomic_load_explicit(&r->head, memory_order_acquire)) == tail) sched_yield();
            record_t v = r->slots[tail & MASK];
            if (!valid(&v, i)) _exit(20);
            ++tail;
            atomic_store_explicit(&r->tail, tail, memory_order_release);
        }
        _exit(0);
    }
    uint64_t head = 0, start = now_ns();
    for (uint64_t i = 0; i < RECORDS; ++i) {
        while (head - atomic_load_explicit(&r->tail, memory_order_acquire) == CAPACITY) sched_yield();
        r->slots[head & MASK] = make_record(i);
        ++head;
        atomic_store_explicit(&r->head, head, memory_order_release);
    }
    uint64_t end = now_ns();
    int rc = wait_child(pid);
    *elapsed = end - start;
    munmap(r, sizeof(*r));
    return rc;
}

/* Descriptor-possession check: a pipe FD is a host analogue of an explicit capability. */
static int capability_test(void) {
    int fd[2]; if (pipe(fd)) return 0;
    pid_t allowed = fork();
    if (allowed == 0) { close(fd[0]); char x='A'; _exit(write(fd[1], &x, 1) == 1 ? 0 : 1); }
    int a = wait_child(allowed);
    char x=0; int got = read(fd[0], &x, 1);
    pid_t denied = fork();
    if (denied == 0) { close(fd[1]); errno=0; int rc=fcntl(fd[1], F_GETFD); _exit(rc == -1 && errno == EBADF ? 0 : 1); }
    int d = wait_child(denied);
    close(fd[0]); close(fd[1]);
    return a == 0 && got == 1 && x == 'A' && d == 0;
}

int main(void) {
    uint64_t pipe_ns=0, ring_ns=0;
    if (!capability_test()) { puts("IPC PROCESS LAB: FAIL capability descriptor gate"); return 1; }
    int p = pipe_test(&pipe_ns);
    int r = shared_ring_test(&ring_ns);
    if (p || r) {
        printf("IPC PROCESS LAB: FAIL pipe=%d shared_ring=%d (shared ring -2 means non-lock-free atomics)\n", p, r);
        return 1;
    }
    uint64_t bytes=(uint64_t)RECORDS*sizeof(record_t);
    printf("records=%u payload_bytes=%" PRIu64 " capacity=%u\n", RECORDS, bytes, CAPACITY);
    printf("pipe_copy_mib_s=%.2f elapsed_ms=%.3f\n", mib_s(bytes,pipe_ns),(double)pipe_ns/1e6);
    printf("process_shared_ring_mib_s=%.2f elapsed_ms=%.3f\n", mib_s(bytes,ring_ns),(double)ring_ns/1e6);
    printf("ring_over_pipe_speedup=%.3f\n", (double)pipe_ns/(double)ring_ns);
    puts("capability_fd_allowed=PASS capability_fd_absent=DENIED");
    puts("correctness=PASS atomics=LOCK_FREE process_boundary=FORKED");
    puts("EVIDENCE=Linux host process-shared prototype; not KIRA kernel IPC");
    return 0;
}
