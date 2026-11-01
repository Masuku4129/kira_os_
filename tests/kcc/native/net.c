/* Программа без прав: сетевые вызовы обязаны получить отказ KE_PERM (-12), файлы - отказ -100. */
#include <kira.h>
#include <stdio.h>

int main(void) {
    i64 ip = k_net_dns("10.0.2.2");
    i64 c = k_net_connect(0x0a000202u, 80);
    i64 f = k_file_write("x.txt", "a", 1);
    fputs_out("net: dns=");
    put_dec(ip);
    fputs_out(" connect=");
    put_dec(c);
    fputs_out(" file=");
    put_dec(f);
    putchar('\n');
    puts(ip == KE_PERM && c == KE_PERM && f == -100 ? "net: refused (PASS)" : "net: NOT refused (FAIL)");
    return 0;
}
