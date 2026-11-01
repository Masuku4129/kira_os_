#define ADD(a, b) ((a) + (b))
#define MUL(a, b) ((a) * (b))
_Static_assert(ADD(1, MUL(2, 3)) == 7, "fn");
