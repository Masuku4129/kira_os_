#define A B
#define B C
#define C 42
_Static_assert(A == 42, "nested");
