int counter(void) { static int n = 10; return ++n; }
int arr[5] = {1, 2, 3}; int *ptrs[3] = {&arr[0], &arr[2], arr + 4};
const char *names[] = {"zero", "one", "two"};
struct S { int *p; const char *s; int v; } st = {&arr[1], "hello", 7};
int main(void) {
    counter(); counter();
    int r = counter() + *ptrs[1] + *st.p + st.s[1] + names[2][1] + st.v;
    ptrs[0][4] = 9;
    return r + arr[4] + (int)(ptrs[2] - ptrs[0]);
}
