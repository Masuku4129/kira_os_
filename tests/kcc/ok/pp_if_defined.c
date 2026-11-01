#define A
#if defined(A) && defined A && !defined(B)
int ok;
#endif
#if !defined B || 0
int ok2;
#endif
