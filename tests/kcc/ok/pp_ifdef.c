#define X
#ifdef X
int a;
#else
int a_bad(;
#endif
#ifndef X
int b_bad(;
#endif
#ifdef Y
int c_bad(;
#endif
