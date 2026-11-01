#define OP(a, b) a ## b
int f(int x) { x OP(+, +); x OP(<<, =) 1; return x; }
