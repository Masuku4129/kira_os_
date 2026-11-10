/* SysV/AAPCS aggregate FP arguments are not yet classified by KCC. */
typedef struct { double x, y; } V2;
static double consume(V2 v) { return v.x + v.y; }
int main(void) { V2 v = { 1.0, 2.0 }; return (int)consume(v); }
