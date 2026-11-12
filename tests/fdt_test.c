// Хост-тест разбора дерева устройств: дерево как у QEMU virt (память, 4 ядра, PL011, GIC, таймер), пути, reg с
// разными #address-cells, совместимость по списку строк, испорченные блобы не принимаются.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "fdt.h"
#include "bootinfo.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t sb[4096], st[1024], blob[8192];
static unsigned sn, tn;
static void be(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void w32(uint32_t v) { be(sb + sn, v); sn += 4; }
static void begin(const char* name) { w32(1); unsigned l = strlen(name) + 1; memcpy(sb + sn, name, l); sn += (l + 3) & ~3u; }
static void end(void) { w32(2); }
static uint32_t stroff(const char* n) {
    for (unsigned i = 0; i < tn; i += strlen((char*)st + i) + 1) if (!strcmp((char*)st + i, n)) return i;
    unsigned o = tn; strcpy((char*)st + tn, n); tn += strlen(n) + 1; return o;
}
static void prop(const char* name, const void* v, unsigned len) {
    w32(3); w32(len); w32(stroff(name));
    memcpy(sb + sn, v, len); sn += (len + 3) & ~3u;
}
static void pstr(const char* name, const char* s, unsigned len) { prop(name, s, len); }
static void pcells(const char* name, const uint32_t* c, unsigned n) { uint8_t b[64]; for (unsigned i = 0; i < n; i++) be(b + 4 * i, c[i]); prop(name, b, 4 * n); }
static void p1(const char* name, uint32_t v) { pcells(name, &v, 1); }

static unsigned build(void) {
    sn = tn = 0;
    begin("");
      p1("#address-cells", 2); p1("#size-cells", 2); pstr("compatible", "linux,dummy-virt", 17);
      begin("memory@40000000"); pstr("device_type", "memory", 7);
        { uint32_t r[] = { 0, 0x40000000, 0, 0x40000000 }; pcells("reg", r, 4); } end();
      begin("memory@70000000"); pstr("device_type", "memory", 7);
        { uint32_t r[] = { 0, 0x70000000, 0, 0x20000000 }; pcells("reg", r, 4); } end();
      begin("memory@a0000000"); pstr("device_type", "memory", 7); pstr("status", "disabled", 9);
        { uint32_t r[] = { 0, 0xA0000000, 0, 0x10000000 }; pcells("reg", r, 4); } end();
      begin("cpus"); p1("#address-cells", 1); p1("#size-cells", 0);
        for (int i = 0; i < 4; i++) { char n[16]; sprintf(n, "cpu@%d", i); begin(n); pstr("device_type", "cpu", 4);
          pstr("compatible", "arm,cortex-a72", 15); p1("reg", (uint32_t)i); end(); }
        begin("cpu-map"); end();
      end();
      begin("pl011@9000000"); pstr("compatible", "arm,pl011\0arm,primecell", 24);
        { uint32_t r[] = { 0, 0x09000000, 0, 0x1000 }; pcells("reg", r, 4); } end();
      begin("intc@8000000"); pstr("compatible", "arm,cortex-a15-gic", 19);
        { uint32_t r[] = { 0, 0x08000000, 0, 0x10000, 0, 0x08010000, 0, 0x10000 }; pcells("reg", r, 8); } end();
      begin("timer"); pstr("compatible", "arm,armv8-timer\0arm,armv7-timer", 32);
        { uint32_t r[] = { 1, 13, 0xf04, 1, 14, 0xf04, 1, 11, 0xf04, 1, 10, 0xf04 }; pcells("interrupts", r, 12); } end();
      begin("framebuffer@0"); pstr("compatible", "simple-framebuffer", 19); pstr("status", "disabled", 9);
        { uint32_t r[] = { 0, 0x1000, 0, 0x1000 }; pcells("reg", r, 4); } end();
      begin("framebuffer@3c000000"); pstr("compatible", "simple-framebuffer", 19); pstr("status", "okay", 5);
        { uint32_t r[] = { 0, 0x3c000000, 0, 800 * 600 * 4 }; pcells("reg", r, 4); }
        p1("width", 800); p1("height", 600); p1("stride", 3200); pstr("format", "a8r8g8b8", 9); end();
      begin("chosen"); pstr("stdout-path", "/pl011@9000000", 15); pstr("bootargs", "console=ttyAMA0", 16); end();
    end();
    w32(9);
    unsigned off_struct = 40 + 32, off_str = off_struct + sn, total = off_str + tn;
    memset(blob, 0, sizeof blob);
    be(blob, FDT_MAGIC); be(blob + 4, total); be(blob + 8, off_struct); be(blob + 12, off_str); be(blob + 16, 40);
    be(blob + 20, 17); be(blob + 24, 16); be(blob + 28, 0); be(blob + 32, tn); be(blob + 36, sn);
    /* One firmware-reserved physical range, followed by the mandatory (0,0) terminator. */
    be(blob + 40, 0); be(blob + 44, 0x1000); be(blob + 48, 0); be(blob + 52, 0x2000);
    be(blob + 56, 0); be(blob + 60, 0); be(blob + 64, 0); be(blob + 68, 0);
    memcpy(blob + off_struct, sb, sn); memcpy(blob + off_str, st, tn);
    return total;
}

int main(void) {
    unsigned total = build();
    CHECK(fdt_valid(blob, sizeof blob) && fdt_tree_valid(blob, sizeof blob) && fdt_total_size(blob) == total);
    int root = fdt_root(blob);
    CHECK(root >= 0 && fdt_compatible(blob, root, "linux,dummy-virt"));

    int uart = fdt_find_compatible(blob, -1, "arm,pl011");
    uint64_t a, s;
    CHECK(uart >= 0 && fdt_reg(blob, uart, 0, &a, &s) == 0 && a == 0x09000000 && s == 0x1000);
    CHECK(fdt_compatible(blob, uart, "arm,primecell") && !fdt_compatible(blob, uart, "arm,pl01"));
    int gic = fdt_find_compatible(blob, -1, "arm,cortex-a15-gic");
    CHECK(gic >= 0 && fdt_reg(blob, gic, 1, &a, &s) == 0 && a == 0x08010000 && s == 0x10000);
    CHECK(fdt_reg(blob, gic, 2, &a, &s) != 0);                    /* третьей пары нет */
    int mem = fdt_find_path(blob, "/memory");
    CHECK(mem >= 0 && fdt_reg(blob, mem, 0, &a, &s) == 0 && a == 0x40000000 && s == 0x40000000);
    int cpus = fdt_find_path(blob, "/cpus");
    CHECK(cpus >= 0 && fdt_count_type(blob, cpus, "cpu") == 4);  /* cpu-map не ядро */
    int cpu2 = fdt_find_path(blob, "/cpus/cpu@2");
    uint32_t r32 = 0;
    CHECK(cpu2 >= 0 && fdt_prop_u32(blob, cpu2, "reg", &r32) == 0 && r32 == 2);
    CHECK(fdt_reg(blob, cpu2, 0, &a, &s) == 0 && a == 2 && s == 0);   /* #address-cells 1, #size-cells 0 */
    CHECK(fdt_parent(blob, cpu2) == cpus && fdt_parent(blob, cpus) == root && fdt_parent(blob, root) == -1);
    int tmr = fdt_find_compatible(blob, -1, "arm,armv8-timer");
    uint32_t len = 0;
    const uint8_t* irq = (const uint8_t*)fdt_prop(blob, tmr, "interrupts", &len);
    CHECK(irq && len == 48 && fdt_u32(irq + 12 + 4) == 14);          /* второй: физический таймер EL1 - PPI 14 */
    int ch = fdt_find_path(blob, "/chosen");
    const char* so = (const char*)fdt_prop(blob, ch, "stdout-path", &len);
    CHECK(so && !strcmp(so, "/pl011@9000000"));
    CHECK(fdt_find_path(blob, "/nope") < 0 && fdt_find_compatible(blob, -1, "x,y") < 0);
    CHECK(fdt_find_compatible(blob, uart, "arm,pl011") < 0);          /* второго такого нет */

    fdt_fb_t fb;
    CHECK(fdt_simplefb(blob, &fb) == 0 && fb.base == 0x3c000000 && fb.width == 800 && fb.height == 600 &&
          fb.stride == 3200 && !strcmp(fb.format, "a8r8g8b8"));          /* выключенный узел пропущен */
    uint64_t reserve_base = 0, reserve_size = 0;
    CHECK(fdt_reserve_count(blob, sizeof blob) == 1);
    CHECK(fdt_reserve(blob, sizeof blob, 0, &reserve_base, &reserve_size) == 0 &&
          reserve_base == 0x1000 && reserve_size == 0x2000);
    CHECK(fdt_reserve(blob, sizeof blob, 1, &reserve_base, &reserve_size) != 0);

    /* reg ranges with wrapped 64-bit physical ends are rejected. */
    {
        uint8_t* reg = (uint8_t*)fdt_prop(blob, mem, "reg", &len);
        CHECK(reg && len == 16);
        be(reg, 0xFFFFFFFFu); be(reg + 4, 0xFFFFF000u);
        be(reg + 8, 0); be(reg + 12, 0x2000);
        CHECK(fdt_reg(blob, mem, 0, &a, &s) != 0);
        total = build();
        mem = fdt_find_path(blob, "/memory");
    }

    /* Property-name offsets must terminate inside the strings block. */
    {
        uint32_t so = fdt_u32(blob + 8), sto = fdt_u32(blob + 12), ts = fdt_u32(blob + 32);
        be(blob + so + 16, ts - 1); blob[sto + ts - 1] = 'X';
        CHECK(fdt_valid(blob, sizeof blob));
        CHECK(fdt_prop(blob, fdt_root(blob), "#address-cells", &len) == 0);
        total = build();
    }

    /* Truncated header buffers must be rejected before any later header
       field is read. ASan catches an over-read for every short length. */
    {
        uint8_t tiny_header[39] = {0};
        be(tiny_header, FDT_MAGIC);
        for (uint32_t short_len = 0; short_len < sizeof tiny_header; short_len++) {
            CHECK(!fdt_valid(tiny_header, short_len));
            CHECK(!fdt_tree_valid(tiny_header, short_len));
        }
    }

    /* испорченные блобы */
    uint8_t bad[8192];
    memcpy(bad, blob, sizeof bad); be(bad, 0x12345678); CHECK(!fdt_valid(bad, sizeof bad));
    memcpy(bad, blob, sizeof bad); be(bad + 4, 100000); CHECK(!fdt_valid(bad, sizeof bad));
    memcpy(bad, blob, sizeof bad); be(bad + 4, 0x80000000u); CHECK(!fdt_valid(bad, UINT32_MAX));
    memcpy(bad, blob, sizeof bad); be(bad + 36, total); CHECK(!fdt_valid(bad, sizeof bad));
    memcpy(bad, blob, sizeof bad); be(bad + 12, fdt_u32(bad + 8)); CHECK(!fdt_valid(bad, sizeof bad)); /* strings alias structure */
    memcpy(bad, blob, sizeof bad); be(bad + 16, fdt_u32(bad + 8)); CHECK(!fdt_valid(bad, sizeof bad)); /* reserve map aliases structure */
    memcpy(bad, blob, sizeof bad); be(bad + 60, 1); be(bad + 68, 0x1000); CHECK(!fdt_valid(bad, sizeof bad)); /* missing reserve terminator */
    memcpy(bad, blob, sizeof bad);
    be(bad + fdt_u32(bad + 8) + fdt_u32(bad + 36) - 4, 4); /* replace FDT_END with NOP */
    CHECK(fdt_valid(bad, sizeof bad) && !fdt_tree_valid(bad, sizeof bad));
    memcpy(bad, blob, sizeof bad); be(bad + fdt_u32(bad + 8) + 12, 0x7FFFFFFF);  /* длина первого свойства за пределами */
    CHECK(fdt_valid(bad, sizeof bad) && !fdt_tree_valid(bad, sizeof bad) && fdt_prop(bad, fdt_root(bad), "#address-cells", &len) == 0);
    /* обход с испорченными данными не уходит за блок */
    int depth = 0, n = fdt_root(bad), steps = 0;
    while (n >= 0 && steps < 1000) { n = fdt_next_node(bad, n, &depth); steps++; }
    CHECK(steps < 1000);

    /* The ARM loader adapter coalesces overlapping RAM, skips disabled banks,
       and publishes framebuffer plus bootargs through the common BootInfo ABI. */
    {
        uint8_t handoff[KIRA_BOOTINFO_MAX_SIZE < 4096 ? KIRA_BOOTINFO_MAX_SIZE : 4096];
        uint32_t handoff_size = 0;
        CHECK(kbi_from_fdt(blob, total, 0x800000, 0x100000, 0x200000,
                           handoff, sizeof handoff, &handoff_size) == 0);
        const kbi_header_t* h = (const kbi_header_t*)handoff;
        CHECK(kbi_validate(handoff, handoff_size) == 0);
        CHECK(h->arch == KBI_ARCH_AARCH64 && h->boot_mode == KBI_BOOT_DEVICE_TREE);
        CHECK((h->flags & (KBI_F_FDT | KBI_F_FRAMEBUFFER | KBI_F_CMDLINE)) ==
              (KBI_F_FDT | KBI_F_FRAMEBUFFER | KBI_F_CMDLINE));
        CHECK(h->mmap_count == 1);
        const kbi_mmap_entry_t* mm = kbi_mmap(handoff);
        CHECK(mm[0].base == 0x40000000 && mm[0].length == 0x50000000);
        CHECK(h->fb_phys == 0x3c000000 && h->fb_width == 800 && h->fb_height == 600);
        CHECK(h->cmdline_len == 16 &&
              !strcmp((const char*)handoff + h->cmdline_off, "console=ttyAMA0"));
        CHECK(kbi_from_fdt(blob, total, 0x800001, 0x100000, 0x200000,
                           handoff, sizeof handoff, &handoff_size) != 0);
    }

    printf(fails ? "FDT TESTS FAILED\n" : "fdt: QEMU-virt-like tree, paths, reg cells, compatible lists, cpu count, corrupted blobs passed\n");
    return fails != 0;
}
