#define A 1
#undef A
#define A 2
_Static_assert(A == 2, "redef");
