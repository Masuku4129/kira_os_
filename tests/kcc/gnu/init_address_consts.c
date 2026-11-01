int a[4]; struct S { int x; int y; } s;
int *p1 = a; int *p2 = &a[2]; int *p3 = a + 3; int *p4 = &s.y; long l = (long)&s;
void (*fp)(void); const char *names[] = {"a", "b"}; int (*pa)[4] = &a;
