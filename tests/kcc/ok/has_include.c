#if __has_include(<hdr_basic.h>) && __has_include("hdr_basic.h") && !__has_include(<no_such_header_zz.h>)
int ok_has_include = 1;
#else
#error has_include evaluated wrongly
#endif
#if defined(__has_include) || 1
int tail = 2;
#endif
