#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
int CAT(var, 1) = 3; int XCAT(var, __LINE__);
_Static_assert(CAT(1, 2) == 12, "paste");
