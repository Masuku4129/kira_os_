struct S { int a; char b[3]; struct { int x; } in; };
struct S s = {1, {2, 3}, {4}};
