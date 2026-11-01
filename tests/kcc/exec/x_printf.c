#include <stdio.h>
#include <string.h>
/* printf-семейство libc-lite kcc (на хосте-эталоне - printf системы): числа, ширины, флаги, длины, строки, snprintf с обрезкой */
int main(void) {
    printf("plain text\n");
    printf("d=%d i=%i u=%u neg=%d zero=%d\n", 42, -7, 4000000000u, -2147483647 - 1, 0);
    printf("x=%x X=%X o=%o hex0=%x\n", 255, 48879, 8, 0);
    printf("ld=%ld lu=%lu lld=%lld llu=%llu lx=%lx\n", -5000000000L, 18446744073709551615UL, -9000000000000000000LL, 9000000000000000000ULL, 0xfeedfaceUL);
    printf("hd=%hd hhd=%hhd hu=%hu zu=%zu\n", 70000, 300, 70000, (size_t)12345678901UL);
    printf("[%5d][%-5d][%05d][%+d][%5s][%-5s][%.2s]\n", 42, 42, 42, 5, "ab", "ab", "abcdef");
    printf("[%*d][%-*d][%.*s][%8.3d][%.0d]\n", 6, 7, 6, 7, 3, "abcdef", 5, 0);
    printf("c=%c%c%c s=%s null=%s pct=100%%\n", 'a', 'b', 'c', "str", (char *)0);
    printf("[%08x][%#x][%#o][% d][%10lu][%-10lu|]\n", 0xbeef, 255, 8, 42, 123456789UL, 987654321UL);
    char buf[16];
    int r = snprintf(buf, sizeof buf, "%s-%d", "abcdefghij", 123456);
    printf("snprintf r=%d buf=%s len=%d\n", r, buf, (int)strlen(buf));
    r = snprintf(buf, 4, "%d", 123456);
    printf("trunc r=%d buf=%s\n", r, buf);
    r = snprintf(0, 0, "%d%d", 11, 22);
    printf("count r=%d\n", r);
    printf("%d %d %d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    printf("%s %s %s %s %s %s %s\n", "a", "b", "c", "d", "e", "f", "g");
    return 0;
}
