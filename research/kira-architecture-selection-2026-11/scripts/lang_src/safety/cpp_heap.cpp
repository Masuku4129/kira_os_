#include <vector>
#include <cstdio>
int main(int argc, char **) {
    volatile int n = 8 + (argc > 99); int mode = argc > 1 ? 1 : 0;
    std::vector<int> a(8, 1), b(8, 100);
    for (int i = 0; i <= n + 3; i++) { if (mode) a.at(i) = 7777; else a[i] = 7777; }
    std::printf("b0=%d b1=%d\n", b[0], b[1]);
}
