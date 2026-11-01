void f(void *p) { static void *t = &&l; goto *p; l: ; }
