#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
_Static_assert(sizeof(uint8_t) == 1 && sizeof(int16_t) == 2 && sizeof(uint32_t) == 4 && sizeof(int64_t) == 8, "widths");
_Static_assert(sizeof(size_t) == 8 && sizeof(uintptr_t) == 8, "ptr");
struct S { uint8_t a; uint32_t b; };
_Static_assert(offsetof(struct S, b) == 4, "offsetof");
bool f(const void *p) { return p != NULL && UINT32_MAX > 0; }
