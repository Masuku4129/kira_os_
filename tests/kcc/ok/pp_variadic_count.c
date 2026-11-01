#define NARGS(...) NARGS_(__VA_ARGS__, 5, 4, 3, 2, 1, 0)
#define NARGS_(a, b, c, d, e, n, ...) n
_Static_assert(NARGS(x) == 1, "n1");
_Static_assert(NARGS(x, y, z) == 3, "n3");
