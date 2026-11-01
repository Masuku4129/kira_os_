#define F(a) a
int x = F((3)) + F((4));
int f(void) { return F((1, 2)); }
