/* слабое определение: переопределяется сильным из t_c.c */
__attribute__((weak)) int pick(void) { return 1; }
int unused_b(int x) { return x * 31; }
const char unused_b_data[64] = "never referenced";
