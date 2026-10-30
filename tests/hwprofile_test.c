// Хост-тест профиля железа: разбор CPUID на синтетических значениях разных процессоров + настоящий CPUID этой машины.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "hwprofile.h"
#include "uheap.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void vendor(hw_cpuid_t* r, const char* v) {
    uint32_t w[3];
    memcpy(w, v, 12);
    r->r[HW_L0][1] = w[0]; r->r[HW_L0][3] = w[1]; r->r[HW_L0][2] = w[2];   /* ebx, edx, ecx */
}

static char text[2048];
static void out(const char* s) { strcat(text, s); }

int main(void) {
    hw_profile_t p;

    /* Intel Skylake клиентский: AVX2, SMEP, SMAP, PCID, INVPCID, XSAVE до AVX (832 байта), инвариантный TSC. */
    hw_cpuid_t sk; memset(&sk, 0, sizeof sk);
    vendor(&sk, "GenuineIntel");
    sk.r[HW_L0][0] = 0x16;
    sk.r[HW_L1][0] = 0x506E3;
    sk.r[HW_L1][2] = (1u << 17) | (1u << 21) | (1u << 24) | (1u << 26) | (1u << 28) | (1u << 30);
    sk.r[HW_L1][3] = 1u << 26;
    sk.r[HW_L7][1] = (1u << 0) | (1u << 5) | (1u << 7) | (1u << 10) | (1u << 18) | (1u << 20);
    sk.r[HW_LD][2] = 832;
    sk.r[HW_L15][0] = 2; sk.r[HW_L15][1] = 188; sk.r[HW_L15][2] = 24000000;      /* 24 МГц * 188 / 2 = 2256 МГц */
    sk.r[HW_E1][3] = (1u << 20) | (1u << 26);
    sk.r[HW_E7][3] = 1u << 8;
    sk.r[HW_E8][0] = 39 | (48 << 8);
    hw_decode(&sk, &p);
    CHECK(strcmp(p.vendor, "GenuineIntel") == 0);
    CHECK(p.family == 6 && p.model == 0x5E && p.stepping == 3);
    CHECK(p.has_sse2 && p.has_nx && p.has_page1g && p.has_xsave && p.has_avx && p.has_avx2 && !p.has_avx512f);
    CHECK(p.has_pcid && p.has_invpcid && p.has_smep && p.has_smap && p.has_fsgsbase && p.has_rdrand && p.has_rdseed);
    CHECK(p.has_x2apic && p.has_tsc_deadline && p.has_inv_tsc && !p.in_vm);
    CHECK(p.xsave_max == 832);
    CHECK(p.tsc_khz_leaf == 2256000);
    CHECK(p.phys_bits == 39 && p.virt_bits == 48);

    /* AMD Zen 3: семейство 0x19 = 0xF + расширенное 0xA, модель 0x21 = 1 + расширенная 2. */
    hw_cpuid_t zen; memset(&zen, 0, sizeof zen);
    vendor(&zen, "AuthenticAMD");
    zen.r[HW_L1][0] = 0xA20F10;
    zen.r[HW_L1][2] = (1u << 26) | (1u << 28);
    zen.r[HW_L1][3] = 1u << 26;
    zen.r[HW_L7][1] = (1u << 5) | (1u << 7) | (1u << 16) | (1u << 20);
    zen.r[HW_LD][2] = 2696;
    hw_decode(&zen, &p);
    CHECK(strcmp(p.vendor, "AuthenticAMD") == 0);
    CHECK(p.family == 0x19 && p.model == 0x21 && p.stepping == 0);
    CHECK(p.has_avx && p.has_avx2 && p.has_avx512f && p.xsave_max == 2696);
    CHECK(p.tsc_khz_leaf == 0);                                                  /* лист 0x15 пуст - процессор не сообщает */

    /* QEMU TCG: виртуальная машина, без AVX, но с XSAVE-битом выключенным; NX есть. */
    hw_cpuid_t q; memset(&q, 0, sizeof q);
    vendor(&q, "GenuineIntel");
    q.r[HW_L1][0] = 0x663;
    q.r[HW_L1][2] = 1u << 31;
    q.r[HW_L1][3] = 1u << 26;
    q.r[HW_E1][3] = 1u << 20;
    hw_decode(&q, &p);
    CHECK(p.in_vm && p.has_sse2 && p.has_nx && !p.has_xsave && !p.has_avx && !p.has_page1g);
    CHECK(p.xsave_max == 0 && p.family == 6 && p.model == 6 && p.stepping == 3);

    /* AVX без XSAVE ничего не значит: состояние нечем сохранять. Размер области без XSAVE не сообщается. */
    hw_cpuid_t bad = q;
    bad.r[HW_L1][2] |= 1u << 28;
    bad.r[HW_L7][1] = (1u << 5) | (1u << 16);
    bad.r[HW_LD][2] = 2696;
    hw_decode(&bad, &p);
    CHECK(!p.has_avx && !p.has_avx2 && !p.has_avx512f && p.xsave_max == 0);

    /* Пустые листы: ничего не заявлено. */
    hw_cpuid_t zero; memset(&zero, 0, sizeof zero);
    hw_decode(&zero, &p);
    CHECK(p.vendor[0] == 0 && !p.has_sse2 && !p.has_nx && !p.has_smep && p.xsave_max == 0 && p.tsc_khz_leaf == 0);

    /* Описание: строки целы, пределы совпадают с политикой кучи. */
    hw_decode(&sk, &p);
    p.cpus = 8; p.ram_kb = 16ULL * 1024 * 1024; p.tsc_khz = 2255998;
    text[0] = 0;
    hw_describe(&p, out);
    CHECK(strstr(text, "GenuineIntel family 6 model 94 stepping 3, bare metal"));
    CHECK(strstr(text, "cores (ACPI): 8, RAM 16384 MB, address bits 39/48"));
    CHECK(strstr(text, "avx2") && strstr(text, "xsave(832B)") && strstr(text, "smap") && strstr(text, "invariant-tsc"));
    CHECK(strstr(text, "TSC 2255 MHz (measured), CPU reports 2256 MHz"));
    CHECK(strstr(text, "program heap up to 8192 MB per program, kernel keeps 1024 MB free"));
    p.tsc_khz = 0; p.tsc_khz_leaf = 0;
    text[0] = 0;
    hw_describe(&p, out);
    CHECK(strstr(text, "TSC not measured yet"));

    /* Настоящий CPUID этого компьютера: x86-64 гарантирует SSE2 и 12 печатных знаков производителя. */
    hw_init(0, 4096);
    const hw_profile_t* h = hw_get();
    CHECK(h->has_sse2 && h->has_nx);
    CHECK(strlen(h->vendor) == 12);
    CHECK(h->cpus == 1 && h->ram_kb == 4096);                                    /* ноль ядер из ACPI превращается в 1 */
    hw_init(6, 1ULL << 22);
    CHECK(hw_get()->cpus == 6);
    hw_set_tsc_khz(3000000);
    CHECK(hw_get()->tsc_khz == 3000000);

    if (fails) { printf("hwprofile: %d FAILED\n", fails); return 1; }
    printf("hwprofile: CPUID decoding (Intel, AMD, VM, empty leaves, AVX without XSAVE), description, real CPUID passed\n");
    return 0;
}
