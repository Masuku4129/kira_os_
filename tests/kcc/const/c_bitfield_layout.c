struct B { unsigned a : 3; unsigned b : 5; unsigned c : 24; unsigned d : 1; };
struct C { char x; int y : 4; char z; };
struct D { int a : 20; int b : 20; int c : 20; };
long long c1 = sizeof(struct B);
long long c2 = sizeof(struct C);
long long c3 = sizeof(struct D);
long long c4 = __builtin_offsetof(struct C, z);
