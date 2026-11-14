#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include "prog_queue.h"
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct { prog_queue_t* q; const char* name; int status; uint32_t id; } submit_arg_t;
static void* submit_worker(void* p) {
    submit_arg_t* a = (submit_arg_t*)p;
    a->status = prog_queue_submit(a->q, a->name, 0, &a->id);
    return 0;
}
int main(void) {
    prog_queue_t q = PROG_QUEUE_INIT;
    uint32_t id = 0, taken = 0; int result = 0;
    char name[PROG_QUEUE_NAME_CAP], file[PROG_QUEUE_FILE_CAP];
    CHECK(prog_queue_submit(&q, 0, 0, &id) == PROG_REQUEST_INVALID);
    CHECK(prog_queue_submit(&q, "paint", 0, 0) == PROG_REQUEST_INVALID);
    CHECK(prog_queue_submit(&q, "", 0, &id) == PROG_REQUEST_INVALID);
    CHECK(prog_queue_submit(&q, "../paint", 0, &id) == PROG_REQUEST_INVALID);
    CHECK(prog_queue_submit(&q, "Paint", 0, &id) == PROG_REQUEST_INVALID);
    char max_name[KFS_NAME_MAX + 1]; memset(max_name, 'a', KFS_NAME_MAX); max_name[KFS_NAME_MAX] = 0;
    char long_name[KFS_NAME_MAX + 2]; memset(long_name, 'a', KFS_NAME_MAX + 1); long_name[KFS_NAME_MAX + 1] = 0;
    CHECK(prog_queue_submit(&q, long_name, 0, &id) == PROG_REQUEST_INVALID);
    char max_file[PROG_QUEUE_FILE_CAP]; memset(max_file, 'x', PROG_QUEUE_FILE_CAP - 1); max_file[PROG_QUEUE_FILE_CAP - 1] = 0;
    CHECK(prog_queue_submit(&q, max_name, max_file, &id) == PROG_REQUEST_ACCEPTED && id != 0);
    CHECK(prog_queue_submit(&q, "paint", 0, &taken) == PROG_REQUEST_BUSY);
    CHECK(prog_queue_take(&q, name, file, &taken) == 1);
    CHECK(taken == id && strcmp(name, max_name) == 0 && strcmp(file, max_file) == 0);
    CHECK(prog_queue_submit(&q, "paint", 0, &taken) == PROG_REQUEST_BUSY); /* RUNNING is still owned */
    CHECK(prog_queue_take(&q, name, file, &taken) == 0);
    CHECK(prog_queue_take_result(&q, id, &result) == PROG_RESULT_PENDING);
    CHECK(prog_queue_complete(&q, id + 1, 42) == -1);
    CHECK(prog_queue_complete(&q, id, -2) == 0);
    CHECK(prog_queue_submit(&q, "paint", 0, &taken) == PROG_REQUEST_BUSY);
    CHECK(prog_queue_take_result(&q, id + 1, &result) == PROG_RESULT_UNKNOWN);
    CHECK(prog_queue_take_result(&q, id, &result) == PROG_RESULT_COMPLETE && result == -2);
    CHECK(prog_queue_take_result(&q, id, &result) == PROG_RESULT_UNKNOWN);
    char long_file[PROG_QUEUE_FILE_CAP + 1]; memset(long_file, 'p', PROG_QUEUE_FILE_CAP); long_file[PROG_QUEUE_FILE_CAP] = 0;
    CHECK(prog_queue_submit(&q, "view", long_file, &id) == PROG_REQUEST_INVALID);
    CHECK(prog_queue_submit(&q, "view", 0, &id) == PROG_REQUEST_ACCEPTED);
    CHECK(prog_queue_take(&q, name, file, &taken) == 1 && taken == id && file[0] == 0);
    CHECK(prog_queue_complete(&q, id, 123) == 0);
    CHECK(prog_queue_take_result(&q, id, &result) == PROG_RESULT_COMPLETE && result == 123);
    enum { THREADS = 12 }; pthread_t th[THREADS]; submit_arg_t args[THREADS];
    for (int i = 0; i < THREADS; i++) {
        args[i].q = &q; args[i].name = (i & 1) ? "paint" : "plasma"; args[i].status = 99; args[i].id = 0;
        CHECK(pthread_create(&th[i], 0, submit_worker, &args[i]) == 0);
    }
    int accepted = 0, busy = 0; uint32_t concurrent_id = 0;
    for (int i = 0; i < THREADS; i++) {
        CHECK(pthread_join(th[i], 0) == 0);
        if (args[i].status == PROG_REQUEST_ACCEPTED) { accepted++; concurrent_id = args[i].id; }
        else if (args[i].status == PROG_REQUEST_BUSY) busy++;
    }
    CHECK(accepted == 1 && busy == THREADS - 1);
    CHECK(prog_queue_take(&q, name, file, &taken) == 1 && taken == concurrent_id);
    CHECK(prog_queue_complete(&q, concurrent_id, 77) == 0);
    CHECK(prog_queue_take_result(&q, concurrent_id, &result) == PROG_RESULT_COMPLETE && result == 77);
    printf("prog queue: validation, exact bounds, busy/accepted, one-time completion and %d-way contention passed\n", THREADS);
    return 0;
}
