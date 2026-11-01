int cnt;
int t(int v) { cnt = cnt * 10 + v; return v; }
int main(void) {
    int r = 0;
    r += t(0) && t(1); r += t(2) || t(3); r += t(0) || t(0); r += t(1) && t(2);
    r += t(1) ? t(2) : t(3); r += t(0) ? t(4) : t(5);
    r += (t(6), t(7));
    int *p = 0; r += p && *p; r += !p || *p;
    return cnt % 100000 + r;
}
