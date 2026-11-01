// expect: inline asm template 'mfence' is not supported on aarch64
int main(void) {
    __asm__ volatile ("mfence");
    return 0;
}
