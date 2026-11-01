int (*p)[4]; int f(int (*q)[4]) { return (*q)[2] + p[1][3]; }
