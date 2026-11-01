enum E { A = 1, B = 5, C, D = C * 2 };
struct S { char c; long l; };
int main(void) {
    int arr[D]; enum E e = C;
    return (int)(sizeof arr / sizeof arr[0]) + e + (int)sizeof(struct S) + (int)sizeof(enum E) + A + B + (int)sizeof(long long);
}
