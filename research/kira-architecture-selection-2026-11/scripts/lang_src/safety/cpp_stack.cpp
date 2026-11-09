#include <array>
#include <cstdio>
int main(int argc, char **) {
    volatile int n = 8 + (argc > 99); int guard = 1111;
    std::array<int, 8> a{}; int mode = argc > 1 ? 1 : 0;   /* без аргументов - operator[], с аргументом - at() */
    for (int i = 0; i <= n + 3; i++) { if (mode) a.at(i) = 7777; else a[i] = 7777; }
    std::printf("guard=%d a0=%d\n", guard, a[0]);
}
