int a[4] = {5, 6, 7, 8};
int *pick(int c) { return c ? &a[1] : &a[3]; }
int main(void) {
    int r = *pick(1) + *pick(0);
    int *p = a + 2, *q = a + 1;
    r += (p > q) * 100 + (p == q) * 7 + (p != q) * 3 + (p >= q) + (q <= p) + (q < p);
    char *s = r > 0 ? "pos" : "neg";
    return r + s[0] + (r > 1000 ? 1 : r > 100 ? 2 : 3);
}
