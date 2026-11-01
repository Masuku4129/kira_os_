struct I { char x; long y; };
struct O { int a; struct I in; char arr[3]; struct I ins[2]; };
long long c1 = __builtin_offsetof(struct O, in);
long long c2 = __builtin_offsetof(struct O, in.y);
long long c3 = __builtin_offsetof(struct O, arr[2]);
long long c4 = __builtin_offsetof(struct O, ins[1].y);
long long c5 = sizeof(struct O);
