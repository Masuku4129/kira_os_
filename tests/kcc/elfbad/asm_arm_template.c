// expect: inline asm template 'dmb ish' is not supported on x86_64
int main(void) {
    int x = 1;
    __asm__ volatile ("dmb ish");
    return x;
}
