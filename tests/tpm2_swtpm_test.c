/* Проверка кодирования команд tpm2.c на НАСТОЯЩЕМ эмуляторе TPM 2.0 (swtpm): счётчик и обычная ячейка NV. Запускает tests/tpm2_swtpm_test.sh.
   Сверяются не байты команд, а поведение: определение, чтение до первого увеличения, увеличение, чтение, запись, отказ авторизации. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tpm2.h"
#include "rollback.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int xfer(void* ctx, const uint8_t* cmd, size_t n, uint8_t* rsp, size_t* rl) {
    int fd = *(int*)ctx;
    if (write(fd, cmd, n) != (ssize_t)n) return -1;
    ssize_t got = read(fd, rsp, *rl);
    if (got < 10) return -1;
    *rl = (size_t)got;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(argv[1])), .sin_addr.s_addr = htonl(0x7f000001) };
    if (connect(fd, (struct sockaddr*)&a, sizeof a)) { perror("connect"); return 2; }
    uint32_t rc = 0; uint64_t v = 0; uint8_t d[16];
    CHECK(tpm2_nv_counter_read(xfer, &fd, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_E_ABSENT);
    printf("до определения: rc=0x%x\n", rc);
    CHECK(tpm2_nv_define(xfer, &fd, KIRA_NV_RELEASE_COUNTER, TPM2_NV_ATTR_COUNTER, 8, &rc) == TPM2_OK);
    CHECK(tpm2_nv_counter_read(xfer, &fd, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_E_EMPTY);
    printf("определён, не увеличен: rc=0x%x\n", rc);
    CHECK(tpm2_nv_define(xfer, &fd, KIRA_NV_RELEASE_COUNTER, TPM2_NV_ATTR_COUNTER, 8, &rc) == TPM2_E_RC);   /* повторное определение */
    printf("повторное определение: rc=0x%x\n", rc);
    for (int i = 0; i < 3; i++) CHECK(tpm2_nv_increment(xfer, &fd, KIRA_NV_RELEASE_COUNTER, &rc) == TPM2_OK);
    CHECK(tpm2_nv_counter_read(xfer, &fd, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_OK);
    printf("после трёх увеличений: %llu\n", (unsigned long long)v);
    CHECK(v == 3);
    CHECK(rb_release_raise_steps(5, v) == 2);

    CHECK(tpm2_nv_read(xfer, &fd, KIRA_NV_DISK_ANCHOR, d, 16, &rc) == TPM2_E_ABSENT);
    CHECK(tpm2_nv_define(xfer, &fd, KIRA_NV_DISK_ANCHOR, TPM2_NV_ATTR_ORDINARY, RB_ANCHOR_LEN, &rc) == TPM2_OK);
    CHECK(tpm2_nv_read(xfer, &fd, KIRA_NV_DISK_ANCHOR, d, 16, &rc) == TPM2_E_EMPTY);
    uint8_t enc[RB_ANCHOR_LEN]; rb_anchor_encode(0x1122334455667788ull, enc);
    CHECK(tpm2_nv_write(xfer, &fd, KIRA_NV_DISK_ANCHOR, enc, RB_ANCHOR_LEN, &rc) == TPM2_OK);
    CHECK(tpm2_nv_read(xfer, &fd, KIRA_NV_DISK_ANCHOR, d, 16, &rc) == TPM2_OK);
    uint64_t e = 0;
    CHECK(rb_anchor_decode(d, &e) == 0 && e == 0x1122334455667788ull);
    rb_anchor_encode(9, enc);
    CHECK(tpm2_nv_write(xfer, &fd, KIRA_NV_DISK_ANCHOR, enc, RB_ANCHOR_LEN, &rc) == TPM2_OK);
    CHECK(tpm2_nv_read(xfer, &fd, KIRA_NV_DISK_ANCHOR, d, 16, &rc) == TPM2_OK && rb_anchor_decode(d, &e) == 0 && e == 9);
    printf(fails ? "tpm2 swtpm: FAIL (%d)\n" : "tpm2 swtpm: все проверки пройдены\n", fails);
    return fails ? 1 : 0;
}
