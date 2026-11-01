// expect: unsupported alignment 32
int main(void) {
    char buf[8] __attribute__((aligned(32)));
    buf[0] = 1;
    return buf[0];
}
