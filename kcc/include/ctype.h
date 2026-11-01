/* kcc libc-lite: ctype.h (локаль C, ASCII) */
#ifndef KCC_CTYPE_H
#define KCC_CTYPE_H
static int isdigit(int c) { return c >= '0' && c <= '9'; }
static int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static int islower(int c) { return c >= 'a' && c <= 'z'; }
static int isalpha(int c) { return isupper(c) || islower(c); }
static int isalnum(int c) { return isalpha(c) || isdigit(c); }
static int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int toupper(int c) { return islower(c) ? c - 32 : c; }
static int tolower(int c) { return isupper(c) ? c + 32 : c; }
#endif
