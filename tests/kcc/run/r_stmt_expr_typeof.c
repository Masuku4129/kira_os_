int main(void) {
    typeof(1 + 1L) x = 5;
    int r = ({ int t = 3; int u = t * x; u + 1; });
    r += ({ int s = 0; for (int i = 0; i < 4; i++) s += i; s; });
    typeof(r) y = ({ r > 10 ? 1 : 2; });
    return r + y + (int)sizeof(x);
}
