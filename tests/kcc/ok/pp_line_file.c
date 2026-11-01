#line 100 "other.c"
int line_is = __LINE__;
_Static_assert(__LINE__ == 101, "line");
#line 5
_Static_assert(__LINE__ == 5, "line2");
const char *f(void) { return __FILE__; }
