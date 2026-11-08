#include "mind_archive_policy.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

int main(void) {
    mind_archive_policy_item_t items[] = {
        { 1, 10, 1 }, { 2, 90, 1 }, { 3, 5, 1 },
        { 4, 1, 1 }, { 5, 0, 0 }, { 6, 100, 1 }
    };
    /* Generation 6 is being written; generation 4 is the previous newest
       valid snapshot. Neither may be selected, even when its score is low. */
    CHECK(mind_archive_choose_victim(items, 6, 6, 4) == 2);
    items[0].score = 90;
    items[2].score = 90;
    CHECK(mind_archive_choose_victim(items, 6, 6, 4) == 0); /* equal score -> oldest */
    items[0].valid = 0;
    items[1].valid = 0;
    items[2].valid = 0;
    CHECK(mind_archive_choose_victim(items, 6, 6, 4) == -1); /* fail closed */
    CHECK(mind_archive_choose_victim(items, 0, 6, 4) == -1);
    puts("Mind archive policy: preserve current and last-good; lowest-score eviction; oldest tie; fail-closed passed");
    return 0;
}
