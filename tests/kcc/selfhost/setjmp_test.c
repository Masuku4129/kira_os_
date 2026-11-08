/* setjmp/longjmp - встроенные функции kcc (x86-64). Печатает код выхода: 0 - всё верно. */
typedef unsigned long jb_t[32];
int setjmp(jb_t env);
void longjmp(jb_t env, int val);

static jb_t env;
static int depth;

static void deep(int n) {
    depth++;
    if (n == 0) longjmp(env, 7);
    deep(n - 1);
}

int main(void) {
    int r = setjmp(env);
    if (r == 0) {
        deep(5);
        return 1;                       /* сюда попасть нельзя */
    }
    if (r != 7 || depth != 6) return 2;
    int z = setjmp(env);
    if (z == 0) longjmp(env, 0);        /* значение 0 превращается в 1 */
    if (z != 1) return 3;
    return 0;
}
