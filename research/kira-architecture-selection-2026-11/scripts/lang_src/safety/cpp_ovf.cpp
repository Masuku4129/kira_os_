#include <cstdio>
int main(int argc, char **) {
    volatile int x = 2147483647 - (argc > 99);
    int y = x + 1; bool t = (x + 1 > x);
    std::printf("y=%d (x+1>x)=%d\n", y, (int)t);
}
