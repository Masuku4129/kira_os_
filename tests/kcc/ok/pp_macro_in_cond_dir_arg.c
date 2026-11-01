#define COND(x) ((x) > 2 ? (x) : 2)
_Static_assert(COND(5) == 5 && COND(1) == 2, "cond");
