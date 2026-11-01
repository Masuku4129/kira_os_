union U { char c; long l; short s[5]; };
struct W { union U u; char t; };
long long c1 = sizeof(union U);
long long c2 = sizeof(struct W);
long long c3 = __builtin_offsetof(struct W, t);
