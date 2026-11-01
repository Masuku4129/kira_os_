struct B { unsigned a : 3; int b : 5; unsigned c : 7; int d : 20; unsigned char e; };
int main(void) {
    struct B s = {0};
    s.a = 9; s.b = -3; s.c = 100; s.d = -1000; s.e = 200;
    int r = s.a + s.b * 10 + s.c * 100;
    s.a++; s.b--; s.c += 40; s.d *= 2; s.e = s.e + 100;
    r += s.a * 7 + s.b * 3 + s.c + s.d + s.e;
    s.b = 20; r += s.b;
    int t = (s.a = 12) + 1;
    return r + t + (int)sizeof(struct B);
}
