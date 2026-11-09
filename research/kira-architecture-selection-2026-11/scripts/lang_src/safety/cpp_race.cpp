#include <thread>
#include <cstdio>
static long counter;                               /* не atomic, без мьютекса */
int main() {
    auto w = [] { for (int i = 0; i < 2000000; i++) { volatile long c = counter; counter = c + 1; } };
    std::thread a(w), b(w); a.join(); b.join();
    std::printf("counter=%ld (ожидалось 4000000)\n", counter);
}
