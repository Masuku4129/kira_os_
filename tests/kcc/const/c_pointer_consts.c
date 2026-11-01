long long c1 = (long)(char *)0;
long long c2 = (long)((char *)0 + 5);
long long c3 = (long)((int *)0 + 5);
long long c4 = (long)&((struct { int a; int b; } *)0)->b;
long long c5 = (long)(void *)16;
