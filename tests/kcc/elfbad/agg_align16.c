// expect: unsupported: struct/union with alignment 16 passed by value
struct A { long a; } __attribute__((aligned(16)));
static long f(struct A x) { return x.a; }
int main(void) { struct A v = { 1 }; return (int)f(v); }
