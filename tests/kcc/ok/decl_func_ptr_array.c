int a(void); int b(void);
int (*tab[2])(void) = {a, b};
int f(void) { return tab[1](); }
