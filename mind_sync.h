#ifndef MIND_SYNC_H
#define MIND_SYNC_H

/* Single-slot request mailboxes shared by UI/console callers and the kernel Mind tick.
   A request arriving after the tick takes a slot remains pending for the next tick. */
static inline int mind_sync_take_int(int* slot, int empty_value) {
    return __atomic_exchange_n(slot, empty_value, __ATOMIC_ACQ_REL);
}

static inline void mind_sync_store_int(int* slot, int value) {
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
}

#endif
