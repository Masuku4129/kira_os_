#ifndef MIND_ARCHIVE_POLICY_H
#define MIND_ARCHIVE_POLICY_H

#include <stdint.h>

/* Candidate metadata only: the caller validates records before marking them valid. */
typedef struct {
    uint32_t generation;
    uint32_t score;
    uint8_t valid;
} mind_archive_policy_item_t;

/*
 * Keep both the snapshot being written and the newest previously valid
 * snapshot. Among all other valid snapshots, evict the lowest score; ties
 * evict the oldest generation. Return -1 rather than weakening protection.
 */
static inline int mind_archive_choose_victim(
    const mind_archive_policy_item_t* items,
    unsigned count,
    uint32_t current_generation,
    uint32_t protected_generation)
{
    int victim = -1;
    for (unsigned i = 0; i < count; ++i) {
        const mind_archive_policy_item_t* item = &items[i];
        if (!item->valid || item->generation == current_generation ||
            item->generation == protected_generation)
            continue;
        if (victim < 0 || item->score < items[victim].score ||
            (item->score == items[victim].score &&
             item->generation < items[victim].generation))
            victim = (int)i;
    }
    return victim;
}

#endif
