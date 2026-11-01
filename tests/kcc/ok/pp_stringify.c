#define STR(x) #x
#define XSTR(x) STR(x)
#define V 12
_Static_assert(sizeof(STR(a  +   b)) == 6, "str1");
_Static_assert(sizeof(XSTR(V)) == 3, "str2");
_Static_assert(sizeof(STR("q\n")) == 6, "str3");
