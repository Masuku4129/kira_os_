int add3(int x) { return x + 3; }
const char *const names[] = { "alpha", "beta", "gamma" };
int nnames = 3;
static int sq(int x) { return x * x; }
static int neg(int x) { return -x; }
int (*const ops[])(int) = { sq, neg, add3 };
int data_val = 41;
int bss_counter;
int bump(void) { return ++bss_counter; }
