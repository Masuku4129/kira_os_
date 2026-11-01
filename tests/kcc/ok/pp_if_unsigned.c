#if -1 > 0u
int ok;
#else
int bad(;
#endif
#if (0u - 1) > 0 && 18446744073709551615u == -1
int ok2;
#endif
