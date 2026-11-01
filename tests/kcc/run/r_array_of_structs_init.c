struct E { const char *name; int v; };
struct E tab[] = { {"a", 1}, {"bb", 2}, [4] = {"eee", 5}, {"f", 6} };
int len(const char *s) { int n = 0; while (s[n]) n++; return n; }
int main(void) {
    int s = 0;
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) s = s * 3 + tab[i].v + (tab[i].name ? len(tab[i].name) : 0);
    return s + (int)(sizeof tab / sizeof tab[0]);
}
