#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include "../mind_sync.h"

#define REQUESTS 1000000
static int mailbox = -1;
static int producer_done;
static int failed;

static void* producer(void* unused) {
    (void)unused;
    for (int i = 1; i <= REQUESTS; ++i) mind_sync_store_int(&mailbox, i);
    __atomic_store_n(&producer_done, 1, __ATOMIC_RELEASE);
    return 0;
}

static void* consumer(void* unused) {
    (void)unused;
    int previous = 0;
    for (;;) {
        int value = mind_sync_take_int(&mailbox, -1);
        if (value != -1) {
            if (value <= previous || value > REQUESTS) __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
            previous = value;
        }
        if (__atomic_load_n(&producer_done, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&mailbox, __ATOMIC_ACQUIRE) == -1) break;
        if (value == -1) sched_yield();
    }
    if (previous != REQUESTS) __atomic_store_n(&failed, 1, __ATOMIC_RELAXED);
    return 0;
}

int main(void) {
    pthread_t p, c;
    if (pthread_create(&p, 0, producer, 0) || pthread_create(&c, 0, consumer, 0)) {
        fputs("mind-sync: pthread creation failed\n", stderr);
        return 2;
    }
    pthread_join(p, 0);
    pthread_join(c, 0);
    if (__atomic_load_n(&failed, __ATOMIC_RELAXED)) {
        fputs("mind-sync: mailbox ordering/last-request regression\n", stderr);
        return 1;
    }
    puts("mind-sync: atomic request mailbox ordering and final delivery passed");
    return 0;
}
