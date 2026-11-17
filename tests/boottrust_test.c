/* Хост-тест состояния доверенной загрузки в ядре (boottrust.c): что ядро сообщает по trust_flags из BootInfo. */
#include "boottrust.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static char buf[4096];
static void out(const char* s) { strncat(buf, s, sizeof(buf) - strlen(buf) - 1); }
static void out_dec(uint64_t v) { char t[24]; snprintf(t, sizeof t, "%llu", (unsigned long long)v); out(t); }

static const char* report(const boottrust_t* t, int* lines) {
    buf[0] = 0;
    *lines = boottrust_report(t, out, out_dec);
    return buf;
}

int main(void) {
    int n;
    kbi_header_t h;
    memset(&h, 0, sizeof h);

    /* до init и для загрузчика без сведений (BIOS, GRUB): цепочки доверия нет, и ядро так и пишет */
    CHECK(boottrust_get()->reported == 0 && !boottrust_is_trusted(boottrust_get()));
    boottrust_init(0);
    report(boottrust_get(), &n);
    CHECK(n == 1 && strstr(buf, "[KIRA BOOT] Trust chain: the loader reports none"));
    h.boot_mode = KBI_BOOT_MULTIBOOT2;
    boottrust_init(&h);
    CHECK(boottrust_get()->reported == 0);

    /* UEFI, Secure Boot выключен, загрузчик разработки */
    h.boot_mode = KBI_BOOT_UEFI;
    h.trust_flags = KBI_TRUST_SB_KNOWN;
    boottrust_init(&h);
    report(boottrust_get(), &n);
    CHECK(n == 4 && strstr(buf, "Secure Boot: disabled") && strstr(buf, "not checked (development loader)") && strstr(buf, "Trusted boot: no"));
    CHECK(!boottrust_is_trusted(boottrust_get()));

    /* UEFI, Secure Boot включён, загрузчик разработки: подпись ядра не проверялась, доверенной загрузкой это не называется */
    h.trust_flags = KBI_TRUST_SB_KNOWN | KBI_TRUST_SB_ENABLED;
    boottrust_init(&h);
    report(boottrust_get(), &n);
    CHECK(strstr(buf, "Secure Boot: enabled\n") && strstr(buf, "Trusted boot: no"));
    CHECK(!boottrust_is_trusted(boottrust_get()));

    /* неизвестное состояние */
    h.trust_flags = 0;
    boottrust_init(&h);
    report(boottrust_get(), &n);
    CHECK(strstr(buf, "Secure Boot: unknown"));

    /* полная цепочка: Secure Boot, принуждающий загрузчик, подпись проверена, TPM */
    h.trust_flags = KBI_TRUST_SB_KNOWN | KBI_TRUST_SB_ENABLED | KBI_TRUST_ENFORCING | KBI_TRUST_SIG_VERIFIED | KBI_TRUST_MEASURED | KBI_TRUST_CMDLINE_IGNORED;
    h.image_version = 12;
    boottrust_init(&h);
    report(boottrust_get(), &n);
    CHECK(n == 4 && strstr(buf, "Secure Boot: enabled\n") && strstr(buf, "verified by the loader") && strstr(buf, "release 12\n"));
    CHECK(strstr(buf, "PCR 9): yes; firmware command line ignored") && strstr(buf, "Trusted boot: YES"));
    CHECK(boottrust_is_trusted(boottrust_get()));
    CHECK(boottrust_get()->image_version == 12);

    /* Setup Mode (ключи платформы не заведены): подпись ничего не доказывает, доверенной загрузки нет */
    h.trust_flags |= KBI_TRUST_SETUP_MODE;
    boottrust_init(&h);
    report(boottrust_get(), &n);
    CHECK(strstr(buf, "Setup Mode") && strstr(buf, "Trusted boot: no"));
    CHECK(!boottrust_is_trusted(boottrust_get()));

    /* без Secure Boot, но с проверкой подписи загрузчиком (KIRA_REQUIRE_SB=0): честно не «доверенная» */
    h.trust_flags = KBI_TRUST_SB_KNOWN | KBI_TRUST_ENFORCING | KBI_TRUST_SIG_VERIFIED;
    boottrust_init(&h);
    CHECK(!boottrust_is_trusted(boottrust_get()));
    report(boottrust_get(), &n);
    CHECK(strstr(buf, "Secure Boot: disabled") && strstr(buf, "PCR 9): no\n"));

    CHECK(boottrust_report(0, out, out_dec) == 0);
    printf("boottrust: PASS\n");
    return 0;
}
