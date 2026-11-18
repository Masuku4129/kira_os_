/* Образец для проверки формата KIRA IR (docs/architecture/KIRA_IR.md): любая правка вывода kcc -S-ir должна менять и sample.ir. */
struct P { int x; long y; };
extern int ext(int, struct P*);
static int table[3] = { 1, 2, 3 };
const char *name = "kira";
int (*hook)(int, struct P*) = ext;

static int sum(int n) { int s = 0; for (int i = 0; i < n; i++) s += table[i % 3]; return s; }
double scale(double v, float k) { return v * k + 1.0; }
long mix(unsigned char a, short b, struct P p) {
    long r = a + b + p.y;
    if (r > 100 || p.x < 0) return -r;
    __atomic_fetch_add(&p.x, 1, __ATOMIC_SEQ_CST);
    return ext((int)r, &p) + sum(a);
}
