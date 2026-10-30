#include "hwprofile.h"
#include "cpu.h"
#include "uheap.h"

static hw_profile_t profile;

#define B(v, n) (((v) >> (n)) & 1u)

void hw_decode(const hw_cpuid_t* raw, hw_profile_t* o) {
    const uint32_t* l0 = raw->r[HW_L0];
    const uint32_t* l1 = raw->r[HW_L1];
    const uint32_t* l7 = raw->r[HW_L7];
    const uint32_t* ld = raw->r[HW_LD];
    const uint32_t* l15 = raw->r[HW_L15];
    const uint32_t* e1 = raw->r[HW_E1];
    const uint32_t* e7 = raw->r[HW_E7];
    const uint32_t* e8 = raw->r[HW_E8];

    uint32_t v[3] = { l0[1], l0[3], l0[2] };                 /* vendor: ebx, edx, ecx */
    for (int i = 0; i < 12; i++) o->vendor[i] = (char)(v[i / 4] >> (8 * (i % 4)));
    o->vendor[12] = 0;

    uint32_t fam = (l1[0] >> 8) & 0xF, mod = (l1[0] >> 4) & 0xF;
    o->stepping = l1[0] & 0xF;
    o->family = fam + (fam == 0xF ? (l1[0] >> 20) & 0xFF : 0);
    o->model = mod + ((fam == 0x6 || fam == 0xF) ? ((l1[0] >> 16) & 0xF) << 4 : 0);

    o->has_sse2 = B(l1[3], 26);
    o->has_pcid = B(l1[2], 17);
    o->has_x2apic = B(l1[2], 21);
    o->has_tsc_deadline = B(l1[2], 24);
    o->has_xsave = B(l1[2], 26);
    o->has_avx = B(l1[2], 28) && B(l1[2], 26);               /* AVX без XSAVE бесполезен: состояние нечем сохранять */
    o->has_rdrand = B(l1[2], 30);
    o->in_vm = B(l1[2], 31);

    o->has_fsgsbase = B(l7[1], 0);
    o->has_avx2 = B(l7[1], 5) && o->has_avx;
    o->has_smep = B(l7[1], 7);
    o->has_invpcid = B(l7[1], 10);
    o->has_avx512f = B(l7[1], 16) && o->has_avx;
    o->has_rdseed = B(l7[1], 18);
    o->has_smap = B(l7[1], 20);

    o->has_nx = B(e1[3], 20);
    o->has_page1g = B(e1[3], 26);
    o->has_inv_tsc = B(e7[3], 8);
    o->phys_bits = (uint8_t)(e8[0] & 0xFF);
    o->virt_bits = (uint8_t)((e8[0] >> 8) & 0xFF);

    o->xsave_max = o->has_xsave ? ld[2] : 0;                  /* ecx: размер под все поддерживаемые компоненты */

    o->tsc_khz_leaf = 0;
    if (l15[0] && l15[1] && l15[2]) o->tsc_khz_leaf = (uint32_t)((uint64_t)l15[2] * l15[1] / l15[0] / 1000);
}

void hw_collect(hw_cpuid_t* raw) {
    for (int i = 0; i < HW_LEAVES; i++) for (int j = 0; j < 4; j++) raw->r[i][j] = 0;
    static const uint32_t leaf[HW_LEAVES] = { 0, 1, 7, 0xD, 0x15, 0x80000000u, 0x80000001u, 0x80000007u, 0x80000008u };
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    uint32_t max_basic = a;
    raw->r[HW_L0][0] = a; raw->r[HW_L0][1] = b; raw->r[HW_L0][2] = c; raw->r[HW_L0][3] = d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    uint32_t max_ext = a;
    for (int i = HW_L1; i < HW_LEAVES; i++) {
        int ext = leaf[i] >= 0x80000000u;
        if (ext ? leaf[i] > max_ext : leaf[i] > max_basic) continue;   /* лист не поддерживается: остаются нули */
        cpuid(leaf[i], &a, &b, &c, &d);
        raw->r[i][0] = a; raw->r[i][1] = b; raw->r[i][2] = c; raw->r[i][3] = d;
    }
    raw->r[HW_E0][0] = max_ext;
}

void hw_init(uint32_t cpus, uint64_t ram_kb) {
    hw_cpuid_t raw;
    hw_collect(&raw);
    hw_decode(&raw, &profile);
    profile.cpus = cpus ? cpus : 1;
    profile.ram_kb = ram_kb;
}

void hw_set_tsc_khz(uint32_t khz) { profile.tsc_khz = khz; }
const hw_profile_t* hw_get(void) { return &profile; }

/* ---- описание ---- */
typedef struct { char b[160]; unsigned n; } line_t;
static void ls(line_t* l, const char* s) { while (*s && l->n < sizeof l->b - 2) l->b[l->n++] = *s++; }
static void lu(line_t* l, uint64_t v) {
    char t[21]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && l->n < sizeof l->b - 2) l->b[l->n++] = t[--n];
}
static void flush(line_t* l, void (*out)(const char*)) { l->b[l->n++] = '\n'; l->b[l->n] = 0; out(l->b); l->n = 0; }

void hw_describe(const hw_profile_t* p, void (*out)(const char*)) {
    line_t l = { {0}, 0 };
    ls(&l, "[KIRA HW] CPU "); ls(&l, p->vendor); ls(&l, " family "); lu(&l, p->family); ls(&l, " model "); lu(&l, p->model);
    ls(&l, " stepping "); lu(&l, p->stepping); ls(&l, p->in_vm ? ", virtual machine" : ", bare metal"); flush(&l, out);

    ls(&l, "[KIRA HW] cores ("); ls(&l, "ACPI): "); lu(&l, p->cpus); ls(&l, ", RAM "); lu(&l, p->ram_kb / 1024); ls(&l, " MB, address bits ");
    lu(&l, p->phys_bits); ls(&l, "/"); lu(&l, p->virt_bits); flush(&l, out);

    ls(&l, "[KIRA HW] features:");
    if (p->has_sse2) ls(&l, " sse2");
    if (p->has_nx) ls(&l, " nx");
    if (p->has_page1g) ls(&l, " page1g");
    if (p->has_xsave) { ls(&l, " xsave("); lu(&l, p->xsave_max); ls(&l, "B)"); }
    if (p->has_avx) ls(&l, " avx");
    if (p->has_avx2) ls(&l, " avx2");
    if (p->has_avx512f) ls(&l, " avx512f");
    if (p->has_pcid) ls(&l, " pcid");
    if (p->has_invpcid) ls(&l, " invpcid");
    if (p->has_smep) ls(&l, " smep");
    if (p->has_smap) ls(&l, " smap");
    if (p->has_fsgsbase) ls(&l, " fsgsbase");
    if (p->has_rdrand) ls(&l, " rdrand");
    if (p->has_rdseed) ls(&l, " rdseed");
    if (p->has_x2apic) ls(&l, " x2apic");
    if (p->has_tsc_deadline) ls(&l, " tsc-deadline");
    if (p->has_inv_tsc) ls(&l, " invariant-tsc");
    flush(&l, out);

    ls(&l, "[KIRA HW] TSC ");
    if (p->tsc_khz) { lu(&l, p->tsc_khz / 1000); ls(&l, " MHz (measured)"); } else ls(&l, "not measured yet");
    if (p->tsc_khz_leaf) { ls(&l, ", CPU reports "); lu(&l, p->tsc_khz_leaf / 1000); ls(&l, " MHz"); }
    flush(&l, out);

    ls(&l, "[KIRA HW] limits for this machine: program heap up to "); lu(&l, uheap_quota_pages(p->ram_kb) * 4 / 1024);
    ls(&l, " MB per program, kernel keeps "); lu(&l, uheap_reserve_kb(p->ram_kb) / 1024); ls(&l, " MB free"); flush(&l, out);
}
