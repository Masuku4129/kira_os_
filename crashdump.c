#include "crashdump.h"

static void hex16(char* dst, uint64_t v) {
    static const char d[] = "0123456789ABCDEF";
    for (int i = 15; i >= 0; i--) { dst[i] = d[v & 0xF]; v >>= 4; }
}

// Одна строка: до четырёх пар "ИМЯ=значение".
static void line(crash_out_fn out, const char* const* names, const uint64_t* vals, unsigned n) {
    char buf[160];
    unsigned p = 0;
    const char* pre = "[KIRA FATAL] ";
    while (*pre) buf[p++] = *pre++;
    for (unsigned i = 0; i < n; i++) {
        const char* s = names[i];
        while (*s) buf[p++] = *s++;
        buf[p++] = '=';
        hex16(buf + p, vals[i]);
        p += 16;
        buf[p++] = (i + 1 < n) ? ' ' : '\n';
    }
    buf[p] = 0;
    out(buf);
}

void crashdump_print(const uint64_t* f, const crash_regs_t* cr, const uint64_t* stack, crash_out_fn out) {
    {
        const char* nm[] = { "RAX", "RBX", "RCX", "RDX" };
        uint64_t v[] = { f[14], f[13], f[12], f[11] };
        line(out, nm, v, 4);
    }
    {
        const char* nm[] = { "RSI", "RDI", "RBP", "RSP" };
        uint64_t v[] = { f[10], f[9], f[8], f[20] };
        line(out, nm, v, 4);
    }
    {
        const char* nm[] = { "R8 ", "R9 ", "R10", "R11" };
        uint64_t v[] = { f[7], f[6], f[5], f[4] };
        line(out, nm, v, 4);
    }
    {
        const char* nm[] = { "R12", "R13", "R14", "R15" };
        uint64_t v[] = { f[3], f[2], f[1], f[0] };
        line(out, nm, v, 4);
    }
    {
        const char* nm[] = { "RIP", "RFLAGS", "CS", "SS" };
        uint64_t v[] = { f[17], f[19], f[18], f[21] };
        line(out, nm, v, 4);
    }
    if (cr) {
        const char* nm[] = { "CR0", "CR2", "CR3", "CR4" };
        uint64_t v[] = { cr->cr0, cr->cr2, cr->cr3, cr->cr4 };
        line(out, nm, v, 4);
    }
    if (stack) {
        const char* a[] = { "S0", "S1", "S2", "S3" };
        const char* b[] = { "S4", "S5", "S6", "S7" };
        uint64_t va[] = { stack[0], stack[1], stack[2], stack[3] };
        uint64_t vb[] = { stack[4], stack[5], stack[6], stack[7] };
        line(out, a, va, 4);
        line(out, b, vb, 4);
    } else {
        out("[KIRA FATAL] Stack at RSP is not readable (user stack, unaligned or outside the kernel map).\n");
    }
}
