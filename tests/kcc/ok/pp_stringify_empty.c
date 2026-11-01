#define S(x) #x
_Static_assert(sizeof(S()) == 1, "empty");
_Static_assert(sizeof(S( a )) == 2, "trim");
