#ifndef KIRA_PROG_QUEUE_H
#define KIRA_PROG_QUEUE_H
#include <stdint.h>
#include "kfs.h"
#include "spinlock.h"
#include "progs.h"
#define PROG_QUEUE_NAME_CAP (KFS_NAME_MAX + 1)
#define PROG_QUEUE_FILE_CAP 128
typedef enum { PROG_QUEUE_IDLE, PROG_QUEUE_PENDING, PROG_QUEUE_RUNNING, PROG_QUEUE_COMPLETE } prog_queue_state_t;
typedef struct {
    spinlock_t lock;
    char name[PROG_QUEUE_NAME_CAP];
    char file[PROG_QUEUE_FILE_CAP];
    uint32_t next_id, active_id;
    prog_queue_state_t state;
    int result;
} prog_queue_t;
#define PROG_QUEUE_INIT { SPINLOCK_INIT, {0}, {0}, 0, 0, PROG_QUEUE_IDLE, 0 }
int prog_queue_submit(prog_queue_t* q, const char* name, const char* file, uint32_t* request_id);
int prog_queue_take(prog_queue_t* q, char name[PROG_QUEUE_NAME_CAP], char file[PROG_QUEUE_FILE_CAP], uint32_t* request_id);
int prog_queue_complete(prog_queue_t* q, uint32_t request_id, int result);
prog_result_state_t prog_queue_take_result(prog_queue_t* q, uint32_t request_id, int* result);
#endif
