struct S { char a; int b; short c; long d; char e; };
long long c1 = __builtin_offsetof(struct S, a);
long long c2 = __builtin_offsetof(struct S, b);
long long c3 = __builtin_offsetof(struct S, c);
long long c4 = __builtin_offsetof(struct S, d);
long long c5 = __builtin_offsetof(struct S, e);
long long c6 = sizeof(struct S);
