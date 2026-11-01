int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }
int (*tab[3])(int, int) = {add, sub, mul};
int apply(int (*f)(int, int), int a, int b) { return f(a, b); }
int main(void) {
    int s = 0;
    for (int i = 0; i < 3; i++) s = s * 10 + tab[i](7, 3) + (*tab[i])(1, 1) + apply(tab[i], 2, 2);
    int (*p)(int, int) = sub; p = p ? mul : add;
    return s + p(4, 5);
}
