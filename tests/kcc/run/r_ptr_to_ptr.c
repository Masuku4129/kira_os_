const char *words[] = {"alpha", "be", "gamma", 0};
int len(const char *s) { int n = 0; while (s[n]) n++; return n; }
int main(void) {
    const char **pp = words; int s = 0;
    while (*pp) { s = s * 10 + len(*pp); pp++; }
    char *a = "x"; char **b = &a;
    return s + **b + (int)(pp - words);
}
