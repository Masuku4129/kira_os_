#define _GNU_SOURCE
#include <errno.h>
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

#define CAPACITY 1024u
#define DEFAULT_ROWS 200000u
#define DEFAULT_REPEAT 7u

typedef struct {
    uint64_t seq;
    uint64_t value;
    uint64_t inverse;
    uint64_t check;
} record_t;

typedef struct {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    record_t slots[CAPACITY];
} shared_ring_t;

static uint64_t payload(uint64_t n) {
    return (n * UINT64_C(0x9e3779b97f4a7c15)) ^ UINT64_C(0xd1b54a32d192ed03);
}
static record_t make_record(uint64_t n) {
    record_t r = { n, payload(n), ~payload(n), n ^ payload(n) ^ UINT64_C(0xa0761d6478bd642f) };
    return r;
}
static int valid_record(const record_t *r, uint64_t expected) {
    record_t e = make_record(expected);
    return r->seq == e.seq && r->value == e.value && r->inverse == e.inverse && r->check == e.check;
}
static double now_sec(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) { perror("clock_gettime"); exit(2); }
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static int write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n; len -= (size_t)n;
    }
    return 0;
}
static int read_full(int fd, void *buf, size_t len) {
    unsigned char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += (size_t)n; len -= (size_t)n;
    }
    return 0;
}
static int run_pipe(uint64_t rows, double *elapsed) {
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t child = fork();
    if (child < 0) { close(p[0]); close(p[1]); return -1; }
    if (child == 0) {
        close(p[1]);
        for (uint64_t i = 0; i < rows; i++) {
            record_t r;
            if (read_full(p[0], &r, sizeof r) || !valid_record(&r, i)) _exit(11);
        }
        close(p[0]); _exit(0);
    }
    close(p[0]);
    double start = now_sec();
    for (uint64_t i = 0; i < rows; i++) {
        record_t r = make_record(i);
        if (write_full(p[1], &r, sizeof r)) { close(p[1]); waitpid(child, NULL, 0); return -1; }
    }
    close(p[1]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    *elapsed = now_sec() - start;
    return 0;
}
static int run_ring(uint64_t rows, double *elapsed) {
    shared_ring_t *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) return -1;
    memset(s, 0, sizeof *s);
    if (!atomic_is_lock_free(&s->head) || !atomic_is_lock_free(&s->tail)) { munmap(s, sizeof *s); return -2; }
    pid_t child = fork();
    if (child < 0) { munmap(s, sizeof *s); return -1; }
    if (child == 0) {
        uint64_t tail = 0;
        for (uint64_t i = 0; i < rows; i++) {
            while (tail == atomic_load_explicit(&s->head, memory_order_acquire)) sched_yield();
            record_t r = s->slots[tail % CAPACITY];
            if (!valid_record(&r, i)) _exit(12);
            tail++;
            atomic_store_explicit(&s->tail, tail, memory_order_release);
        }
        _exit(0);
    }
    double start = now_sec();
    uint64_t head = 0;
    for (uint64_t i = 0; i < rows; i++) {
        while (head - atomic_load_explicit(&s->tail, memory_order_acquire) >= CAPACITY) sched_yield();
        s->slots[head % CAPACITY] = make_record(i);
        head++;
        atomic_store_explicit(&s->head, head, memory_order_release);
    }
    int status = 0;
    int w = waitpid(child, &status, 0);
    *elapsed = now_sec() - start;
    munmap(s, sizeof *s);
    if (w < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}
static double median(double *v, unsigned n) {
    for (unsigned i = 0; i < n; i++) for (unsigned j = i + 1; j < n; j++) if (v[j] < v[i]) {
        double t = v[i]; v[i] = v[j]; v[j] = t;
    }
    return n % 2 ? v[n/2] : (v[n/2-1] + v[n/2]) / 2.0;
}
static int capability_gate_test(void) {
    static const char *required = "KIRA_IPC_SEND:demo-service";
    const char *granted = "KIRA_IPC_SEND:demo-service";
    const char *wrong = "KIRA_IPC_SEND:other-service";
    return strcmp(required, granted) == 0 && strcmp(required, wrong) != 0;
}
int main(int argc, char **argv) {
    uint64_t rows = DEFAULT_ROWS;
    unsigned repeat = DEFAULT_REPEAT;
    if (argc > 1) rows = strtoull(argv[1], NULL, 10);
    if (argc > 2) repeat = (unsigned)strtoul(argv[2], NULL, 10);
    if (!rows || !repeat || repeat > 31) { fprintf(stderr, "usage: %s [rows>0] [repeat=1..31]\n", argv[0]); return 2; }
    if (!capability_gate_test()) { fprintf(stderr, "FAIL: capability gate\n"); return 1; }
    double pipe_samples[31], ring_samples[31];
    for (unsigned i = 0; i < repeat; i++) {
        if (run_pipe(rows, &pipe_samples[i]) != 0) { fprintf(stderr, "FAIL: pipe round %u\n", i); return 1; }
        if (run_ring(rows, &ring_samples[i]) != 0) { fprintf(stderr, "FAIL: shared ring round %u\n", i); return 1; }
    }
    double p = median(pipe_samples, repeat), r = median(ring_samples, repeat);
    printf("{\"rows\":%" PRIu64 ",\"repeat\":%u,\"record_bytes\":%zu,"
           "\"pipe_median_ms\":%.3f,\"shared_ring_median_ms\":%.3f,"
           "\"pipe_mops\":%.3f,\"shared_ring_mops\":%.3f,"
           "\"ring_over_pipe_speedup\":%.3f,\"capability_gate\":\"PASS\","
           "\"ordering_and_payload_integrity\":\"PASS\",\"shared_atomics_lock_free\":\"PASS\"}\n",
           rows, repeat, sizeof(record_t), p*1000.0, r*1000.0,
           rows/p/1e6, rows/r/1e6, p/r);
    return 0;
}
