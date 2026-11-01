#define S(x) #x
#define ID(x) x
#define K 3
_Static_assert(sizeof(S(K)) == 2, "raw");
_Static_assert(sizeof(S(ID(K))) == 6, "raw2");
