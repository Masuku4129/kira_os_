struct A { char c; } __attribute__((aligned(16)));
int x __attribute__((aligned(8)));
_Static_assert(sizeof(struct A) == 16, "aligned");
