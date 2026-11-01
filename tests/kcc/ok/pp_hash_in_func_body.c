#define MK(name, type) type get_##name(void) { return (type)0; }
MK(int, int) MK(long, long)
int f(void) { return get_int() + (int)get_long(); }
