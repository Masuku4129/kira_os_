#include "prog_queue.h"
static int name_valid(const char* name) {
    if (!name || !name[0]) return 0;
    for (unsigned i = 0; name[i]; i++) {
        char c = name[i];
        if (i >= KFS_NAME_MAX || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}
static int file_length_valid(const char* file) {
    if (!file) return 1;
    unsigned i = 0;
    while (i < PROG_QUEUE_FILE_CAP && file[i]) i++;
    return i < PROG_QUEUE_FILE_CAP;
}
static void copy_string(char* dst, const char* src) {
    unsigned i = 0;
    if (src) while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
int prog_queue_submit(prog_queue_t* q, const char* name, const char* file, uint32_t* request_id) {
    if (!q || !request_id || !name_valid(name) || !file_length_valid(file)) return PROG_REQUEST_INVALID;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (q->state != PROG_QUEUE_IDLE) { spin_unlock_irqrestore(&q->lock, flags); return PROG_REQUEST_BUSY; }
    uint32_t id = q->next_id + 1;
    if (!id) id = 1;
    q->next_id = id; q->active_id = id;
    copy_string(q->name, name); copy_string(q->file, file);
    q->result = 0; q->state = PROG_QUEUE_PENDING; *request_id = id;
    spin_unlock_irqrestore(&q->lock, flags);
    return PROG_REQUEST_ACCEPTED;
}
int prog_queue_take(prog_queue_t* q, char name[PROG_QUEUE_NAME_CAP], char file[PROG_QUEUE_FILE_CAP], uint32_t* request_id) {
    if (!q || !name || !file || !request_id) return 0;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (q->state != PROG_QUEUE_PENDING) { spin_unlock_irqrestore(&q->lock, flags); return 0; }
    copy_string(name, q->name); copy_string(file, q->file);
    *request_id = q->active_id; q->state = PROG_QUEUE_RUNNING;
    spin_unlock_irqrestore(&q->lock, flags);
    return 1;
}
int prog_queue_complete(prog_queue_t* q, uint32_t request_id, int result) {
    if (!q || !request_id) return -1;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (q->state != PROG_QUEUE_RUNNING || q->active_id != request_id) { spin_unlock_irqrestore(&q->lock, flags); return -1; }
    q->result = result; q->state = PROG_QUEUE_COMPLETE;
    spin_unlock_irqrestore(&q->lock, flags);
    return 0;
}
prog_result_state_t prog_queue_take_result(prog_queue_t* q, uint32_t request_id, int* result) {
    if (!q || !request_id || !result) return PROG_RESULT_UNKNOWN;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (q->active_id != request_id || q->state == PROG_QUEUE_IDLE) { spin_unlock_irqrestore(&q->lock, flags); return PROG_RESULT_UNKNOWN; }
    if (q->state != PROG_QUEUE_COMPLETE) { spin_unlock_irqrestore(&q->lock, flags); return PROG_RESULT_PENDING; }
    *result = q->result; q->state = PROG_QUEUE_IDLE; q->active_id = 0;
    q->name[0] = 0; q->file[0] = 0; q->result = 0;
    spin_unlock_irqrestore(&q->lock, flags);
    return PROG_RESULT_COMPLETE;
}
