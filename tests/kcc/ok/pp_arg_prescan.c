#define ID(x) x
#define TWICE(x) ID(x) + ID(x)
_Static_assert(TWICE(ID(2)) == 4, "prescan");
