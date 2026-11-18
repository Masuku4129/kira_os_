/* Хост-тест подписанного манифеста загрузки (bootsig.c): манифест и ключи делает tests/bootsig_test.sh инструментом tools/kirasig.py,
   то есть проверяется совместимость формата между подписывающим инструментом и проверкой в загрузчике.
   Аргументы: pub.bin KIRA.SIG kernel.bin initrd.bin other_pub.bin */
#include "bootsig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("нет файла %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc(sz ? (size_t)sz : 1);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int main(int argc, char** argv) {
    if (argc != 6) { printf("usage: pub sig kernel initrd other_pub\n"); return 2; }
    size_t npub, nsig, nk, ni, nop;
    uint8_t* pub = slurp(argv[1], &npub);
    uint8_t* sig = slurp(argv[2], &nsig);
    uint8_t* ker = slurp(argv[3], &nk);
    uint8_t* ini = slurp(argv[4], &ni);
    uint8_t* opub = slurp(argv[5], &nop);
    CHECK(npub == 32 && nop == 32);
    bootsig_t s;

    /* верный манифест: два файла, номер выпуска 7 */
    CHECK(bootsig_parse_verify(sig, nsig, pub, 0, &s) == BOOTSIG_OK);
    CHECK(s.count == 2 && s.image_version == 7);
    CHECK(strcmp(s.entry[0].name, BOOTSIG_NAME_KERNEL) == 0 && strcmp(s.entry[1].name, BOOTSIG_NAME_INITRD) == 0);
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_KERNEL, ker, nk) == BOOTSIG_OK);
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_INITRD, ini, ni) == BOOTSIG_OK);

    /* сообщение, собранное C-кодом, совпадает с тем, что подписал инструмент: вторая подпись не нужна, достаточно сверить длину */
    uint8_t msg[BOOTSIG_DOMAIN_LEN + BOOTSIG_MAX_LEN];
    CHECK(bootsig_message(&s, msg, sizeof(msg)) == BOOTSIG_DOMAIN_LEN + nsig - BOOTSIG_SIG_LEN);
    CHECK(memcmp(msg + BOOTSIG_DOMAIN_LEN, sig, nsig - BOOTSIG_SIG_LEN) == 0);

    /* откат: порог выше номера выпуска отклоняется, равный и ниже - нет */
    CHECK(bootsig_parse_verify(sig, nsig, pub, 7, &s) == BOOTSIG_OK);
    CHECK(bootsig_parse_verify(sig, nsig, pub, 8, &s) == BOOTSIG_E_ROLLBACK);
    CHECK(s.count == 0 && s.image_version == 0);                   /* при отказе поля не утекают наружу */
    CHECK(bootsig_parse_verify(sig, nsig, pub, 0xFFFFFFFFu, &s) == BOOTSIG_E_ROLLBACK);
    CHECK(bootsig_parse_verify(sig, nsig, pub, 7, &s) == BOOTSIG_OK);

    /* чужой ключ */
    CHECK(bootsig_parse_verify(sig, nsig, opub, 0, &s) == BOOTSIG_E_SIGNATURE);

    /* изменение ЛЮБОГО байта манифеста (тело или подпись) должно отклоняться: подпись, формат или (для резерва) тоже подпись */
    for (size_t i = 0; i < nsig; i++) {
        for (int bit = 0; bit < 8; bit += 7) {                       /* младший и старший биты каждого байта */
            sig[i] ^= (uint8_t)(1u << bit);
            int rc = bootsig_parse_verify(sig, nsig, pub, 0, &s);
            CHECK(rc != BOOTSIG_OK);
            CHECK(s.count == 0);
            sig[i] ^= (uint8_t)(1u << bit);
        }
    }
    CHECK(bootsig_parse_verify(sig, nsig, pub, 0, &s) == BOOTSIG_OK);

    /* усечение и дописывание */
    for (size_t n = 0; n < nsig; n++) CHECK(bootsig_parse_verify(sig, n, pub, 0, &s) != BOOTSIG_OK);
    uint8_t* longer = malloc(nsig + 1);
    memcpy(longer, sig, nsig);
    longer[nsig] = 0;
    CHECK(bootsig_parse_verify(longer, nsig + 1, pub, 0, &s) == BOOTSIG_E_FORMAT);
    free(longer);
    CHECK(bootsig_parse_verify(NULL, nsig, pub, 0, &s) == BOOTSIG_E_ARGS);
    CHECK(bootsig_parse_verify(sig, nsig, NULL, 0, &s) == BOOTSIG_E_ARGS);

    CHECK(bootsig_parse_verify(sig, nsig, pub, 0, &s) == BOOTSIG_OK);

    /* изменение любого байта ядра и initrd: отказ по хэшу; чужой размер: отказ по размеру */
    for (size_t i = 0; i < nk; i++) {
        ker[i] ^= 0x01;
        CHECK(bootsig_check_file(&s, BOOTSIG_NAME_KERNEL, ker, nk) == BOOTSIG_E_HASH);
        ker[i] ^= 0x01;
    }
    for (size_t i = 0; i < ni; i++) {
        ini[i] ^= 0x80;
        CHECK(bootsig_check_file(&s, BOOTSIG_NAME_INITRD, ini, ni) == BOOTSIG_E_HASH);
        ini[i] ^= 0x80;
    }
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_KERNEL, ker, nk - 1) == BOOTSIG_E_SIZE);
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_KERNEL, ker, nk + 1) == BOOTSIG_E_SIZE);
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_KERNEL, ker, nk) == BOOTSIG_OK);
    /* ядро под именем initrd и наоборот - не проходит; лишний файл, которого нет в манифесте, отклоняется */
    CHECK(bootsig_check_file(&s, BOOTSIG_NAME_INITRD, ker, nk) == BOOTSIG_E_SIZE || bootsig_check_file(&s, BOOTSIG_NAME_INITRD, ker, nk) == BOOTSIG_E_HASH);
    CHECK(bootsig_check_file(&s, "EXTRA.BIN", ker, nk) == BOOTSIG_E_UNLISTED);
    CHECK(bootsig_find(&s, "EXTRA.BIN") == NULL && bootsig_find(&s, BOOTSIG_NAME_INITRD) != NULL);

    /* ошибки названы человеческим текстом */
    for (int rc = BOOTSIG_E_HASH; rc <= BOOTSIG_OK; rc++) CHECK(bootsig_strerror(rc)[0] != 0);
    printf("bootsig: PASS (манифест %zu байт, ядро %zu, initrd %zu)\n", nsig, nk, ni);
    free(pub); free(sig); free(ker); free(ini); free(opub);
    return 0;
}
