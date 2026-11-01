#define F(x, y) x + y
#define G(a) F(a, a)
_Static_assert(G(2) == 4, "g");
