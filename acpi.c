#include "acpi.h"
#include "uart.h"

typedef struct { char sig[8]; uint8_t csum; char oem[6]; uint8_t rev; uint32_t rsdt; uint32_t len; uint64_t xsdt; uint8_t ecsum; uint8_t rsv[3]; } __attribute__((packed)) rsdp_t;
typedef struct { char sig[4]; uint32_t len; uint8_t rev; uint8_t csum; char oem[6]; char tid[8]; uint32_t orev; uint32_t cid; uint32_t crev; } __attribute__((packed)) sdt_t;

static acpi_info_t info;
static const void* (*map_hook)(uint64_t phys, uint64_t len);

void acpi_set_map_hook(const void* (*hook)(uint64_t phys, uint64_t len)) { map_hook = hook; }

/* Адрес, по которому таблицу можно читать: ниже max_phys - тот же (тождественная карта), выше - что дал хук ядра
   (таблицы ACPI на ПК с 2+ ГБ лежат у верхней границы ОЗУ, на серверах - и выше 4 ГБ). 0 - не читается. */
static const uint8_t* reach(uint64_t phys, uint64_t len, uint64_t max_phys) {
    if (phys + len < phys) return 0;
    if (phys + len <= max_phys) return (const uint8_t*)(uintptr_t)phys;
    return map_hook ? (const uint8_t*)map_hook(phys, len) : 0;
}

static int sum_ok(const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p; uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s == 0;
}
static int sig_is(const char* s, const char* want) { return s[0]==want[0] && s[1]==want[1] && s[2]==want[2] && s[3]==want[3]; }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int parse_madt(const sdt_t* t, acpi_info_t* o) {
    const uint8_t* b = (const uint8_t*)t;
    if (t->len < 44) return -1;
    o->lapic_addr = rd32(b + 36);
    uint32_t off = 44;
    while (off < t->len) {
        if (t->len - off < 2) return -1;
        uint8_t type = b[off], len = b[off + 1];
        if (len < 2 || len > t->len - off) return -1;
        if (type == 0) {
            if (len < 8) return -1;
            if (rd32(b + off + 4) & 1) {
                uint32_t id = b[off + 3];
                int dup = 0;
                for (uint32_t k = 0; k < o->cpu_count && k < ACPI_MAX_CPU; k++)
                    if (o->cpu_apic_id[k] == id) dup = 1;
                if (!dup) {
                    if (o->cpu_count < ACPI_MAX_CPU) o->cpu_apic_id[o->cpu_count] = id;
                    if (id > o->max_apic_id) o->max_apic_id = id;
                    o->cpu_count++;
                }
            }
        } else if (type == 1) {
            if (len < 12) return -1;
            if (o->ioapic_count < ACPI_MAX_IOAPIC) {
                o->ioapic[o->ioapic_count].id = b[off + 2];
                o->ioapic[o->ioapic_count].addr = rd32(b + off + 4);
                o->ioapic[o->ioapic_count].gsi_base = rd32(b + off + 8);
            }
            o->ioapic_count++;
        } else if (type == 2) {
            if (len < 10) return -1;
            o->irq_override_count++;
            uint8_t src = b[off + 3];
            if (b[off + 2] == 0 && src < 16) {
                o->iso_valid[src] = 1;
                o->iso_gsi[src] = rd32(b + off + 4);
                o->iso_flags[src] = (uint16_t)(b[off + 8] | (b[off + 9] << 8));
            }
        } else if (type == 9) {
            if (len < 16) return -1;
            if (rd32(b + off + 8) & 1) {
                uint32_t id = rd32(b + off + 4);
                int dup = 0;
                for (uint32_t k = 0; k < o->cpu_count && k < ACPI_MAX_CPU; k++)
                    if (o->cpu_apic_id[k] == id) dup = 1;
                if (!dup) {
                    if (o->cpu_count < ACPI_MAX_CPU) o->cpu_apic_id[o->cpu_count] = id;
                    if (id > o->max_apic_id) o->max_apic_id = id;
                    o->cpu_count++;
                }
            }
        } else if (type == 5) {
            if (len < 12) return -1; /* Local APIC Address Override */
            o->lapic_addr = rd64(b + off + 4);
        }
        off += len;
    }
    return 0;
}

/* Число AML в пакете: BytePrefix/WordPrefix/DWordPrefix, Zero, One, Ones. -1 - не число. */
static int aml_int(const uint8_t* p, const uint8_t* e, uint32_t* v, const uint8_t** next) {
    if (!p || !e || !v || !next || p >= e) return -1;
    uint64_t remain = (uint64_t)(e - p);
    if (*p == 0x00) { *v = 0; *next = p + 1; return 0; }
    if (*p == 0x01) { *v = 1; *next = p + 1; return 0; }
    if (*p == 0xFF) { *v = 0xFF; *next = p + 1; return 0; }
    if (*p == 0x0A && remain >= 2) { *v = p[1]; *next = p + 2; return 0; }
    if (*p == 0x0B && remain >= 3) { *v = p[1] | (uint32_t)p[2] << 8; *next = p + 3; return 0; }
    if (*p == 0x0C && remain >= 5) { *v = rd32(p + 1); *next = p + 5; return 0; }
    return -1;
}

/* Decode AML PkgLength and return the exclusive end of its package.
   PkgLength counts its own encoding bytes as well as the package body. */
static int aml_pkg_end(const uint8_t* p, const uint8_t* e, const uint8_t** end) {
    if (!p || !e || p >= e) return -1;
    uint8_t lead = *p;
    uint32_t follow = (uint32_t)(lead >> 6);
    uint32_t length = follow ? (uint32_t)(lead & 0x0F) : (uint32_t)(lead & 0x3F);
    if ((uint64_t)(e - p) < follow + 1u) return -1;
    for (uint32_t i = 0; i < follow; i++)
        length |= (uint32_t)p[1 + i] << (4u + 8u * i);
    if (length < follow + 2u || (uint64_t)(e - p) < length) return -1;
    *end = p + length;
    return 0;
}

int acpi_find_s5(const uint8_t* aml, uint32_t len, uint8_t* a, uint8_t* b) {
    if (!aml || !a || !b || len < 8) return -1;
    const uint8_t* e = aml + len;
    for (uint32_t i = 1; i < len && len - i > 5u; i++) {
        if (aml[i] != '_' || aml[i + 1] != 'S' || aml[i + 2] != '5' || aml[i + 3] != '_') continue;
        /* NameOp (0x08), optionally rooted with '\\'. */
        if (!(aml[i - 1] == 0x08 || (i >= 2 && aml[i - 1] == '\\' && aml[i - 2] == 0x08))) continue;
        const uint8_t* p = aml + i + 4;
        if (p >= e || *p++ != 0x12) continue;                    /* PackageOp */
        const uint8_t* package_end = 0;
        if (aml_pkg_end(p, e, &package_end) != 0) continue;
        uint32_t pkg_length_bytes = 1u + (uint32_t)(*p >> 6);
        p += pkg_length_bytes; /* skip PkgLength encoding; NumElements starts the package body */
        if (p >= package_end) continue;
        uint8_t count = *p++;                                    /* NumElements */
        if (count < 2) continue;
        uint32_t va, vb;
        if (aml_int(p, package_end, &va, &p) != 0 ||
            aml_int(p, package_end, &vb, &p) != 0) continue;
        /* ACPI SLP_TYP fields are three bits wide; reject malformed values. */
        if (va > 7 || vb > 7) continue;
        *a = (uint8_t)va;
        *b = (uint8_t)vb;
        return 0;
    }
    return -1;
}

static void parse_fadt(const sdt_t* t, uint64_t max_phys, acpi_info_t* o) {
    const uint8_t* f = (const uint8_t*)t;
    uint32_t len = t->len;
    if (len < 116) return;
    o->smi_cmd = rd32(f + 48); o->acpi_enable = f[52];
    o->pm1a_cnt = rd32(f + 64); o->pm1b_cnt = rd32(f + 68);
    uint32_t flags = rd32(f + 112);
    if (len >= 111 && f[8] >= 2 && (f[109] & 1)) o->legacy_isa = 1;   /* IAPC_BOOT_ARCH: LEGACY_DEVICES */
    if (len >= 129 && (flags & (1u << 10))) {                    /* RESET_REG_SUP */
        o->reset_space = f[116]; o->reset_addr = rd64(f + 120); o->reset_value = f[128];
        o->reset_valid = o->reset_addr && (o->reset_space == 0 || o->reset_space == 1);
    }
    uint64_t dsdt = rd32(f + 40);
    if (len >= 148 && rd64(f + 140)) dsdt = rd64(f + 140);       /* X_DSDT */
    const sdt_t* d = dsdt ? (const sdt_t*)reach(dsdt, sizeof(sdt_t), max_phys) : 0;
    if (!d || !sig_is(d->sig, "DSDT") || d->len < sizeof(sdt_t)) return;
    uint32_t dl = d->len;
    if (!(d = (const sdt_t*)reach(dsdt, dl, max_phys))) return;
    if (acpi_find_s5((const uint8_t*)d + sizeof(sdt_t), dl - (uint32_t)sizeof(sdt_t), &o->slp_typa, &o->slp_typb) == 0) o->s5_valid = 1;
}

static void parse_table(uint64_t phys, uint64_t max_phys, acpi_info_t* o) {
    const sdt_t* t = phys ? (const sdt_t*)reach(phys, sizeof(sdt_t), max_phys) : 0;
    if (!t) { o->tables_skipped_high++; return; }
    uint32_t tl = t->len;
    if (tl < sizeof(sdt_t) || !(t = (const sdt_t*)reach(phys, tl, max_phys))) { o->tables_skipped_high++; return; }
    if (!sum_ok(t, t->len)) { o->tables_bad++; return; }
    if (sig_is(t->sig, "APIC")) {
        acpi_info_t parsed = *o;
        if (parse_madt(t, &parsed) != 0) { o->tables_bad++; return; }
        *o = parsed; /* malformed MADT must not leak partially parsed topology */
    }
    o->tables_seen++;
    if (sig_is(t->sig, "HPET")) o->has_hpet = 1;
    else if (sig_is(t->sig, "FACP")) parse_fadt(t, max_phys, o);
    else if (sig_is(t->sig, "MCFG") && t->len >= 44 + 16) {
        const uint8_t* e = (const uint8_t*)t + 44;
        o->mcfg_base = rd64(e); o->mcfg_bus_start = e[10]; o->mcfg_bus_end = e[11];
    }
}

int acpi_parse(uint64_t rsdp_phys, uint64_t max_phys, acpi_info_t* o) {
    uint8_t* z = (uint8_t*)o; for (uint32_t i = 0; i < sizeof(*o); i++) z[i] = 0;
    if (rsdp_phys == 0 || rsdp_phys > max_phys || max_phys - rsdp_phys < 20) return -1;
    const rsdp_t* r = (const rsdp_t*)(uintptr_t)rsdp_phys;
    const char magic[8] = {'R','S','D',' ','P','T','R',' '};
    for (int i = 0; i < 8; i++) if (r->sig[i] != magic[i]) return -2;
    if (!sum_ok(r, 20)) return -3;
    o->revision = r->rev;
    int use_x = 0;
    if (r->rev >= 2) {
        if (max_phys - rsdp_phys < sizeof(rsdp_t) || r->len < sizeof(rsdp_t) ||
            r->len > max_phys - rsdp_phys || !sum_ok(r, r->len)) return -4;
        use_x = r->xsdt != 0;
    }
    uint64_t root = use_x ? r->xsdt : r->rsdt;
    const sdt_t* rt = root ? (const sdt_t*)reach(root, sizeof(sdt_t), max_phys) : 0;
    if (!rt) return -5;
    uint32_t rl = rt->len;
    uint32_t esz = use_x ? 8 : 4;
    if (rl < sizeof(sdt_t) || ((rl - sizeof(sdt_t)) % esz) != 0 ||
        !(rt = (const sdt_t*)reach(root, rl, max_phys)) || !sum_ok(rt, rt->len)) return -6;
    uint32_t n = (rt->len - sizeof(sdt_t)) / esz;
    const uint8_t* ents = (const uint8_t*)rt + sizeof(sdt_t);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = use_x ? rd64(ents + i * 8) : rd32(ents + i * 4);
        parse_table(p, max_phys, o);
    }
    o->valid = 1;
    return 0;
}

/* ---------------- таблицы Intel MP ---------------- */

uint64_t acpi_scan(uint64_t from, uint64_t to, const char* sig, unsigned sig_len, unsigned sum_len) {
    for (uint64_t a = (from + 15) & ~15ull; a + 16 <= to; a += 16) {
        const uint8_t* p = (const uint8_t*)(uintptr_t)a;
        unsigned k = 0; while (k < sig_len && p[k] == (uint8_t)sig[k]) k++;
        if (k != sig_len) continue;
        unsigned n = sum_len ? sum_len : (unsigned)p[8] * 16u;
        if (n == 0 || a + n > to) continue;
        if (sum_ok(p, n)) return a;
    }
    return 0;
}

static void mp_add_cpu(acpi_info_t* o, uint32_t id) {
    for (uint32_t k = 0; k < o->cpu_count && k < ACPI_MAX_CPU; k++) if (o->cpu_apic_id[k] == id) return;
    if (o->cpu_count < ACPI_MAX_CPU) o->cpu_apic_id[o->cpu_count] = id;
    if (id > o->max_apic_id) o->max_apic_id = id;
    o->cpu_count++;
}

static int mp_parse_impl(uint64_t fps, uint64_t max_phys, acpi_info_t* o) {
    if (!o || !fps || fps > max_phys || max_phys - fps < 16) return -1;
    const uint8_t* f = (const uint8_t*)(uintptr_t)fps;
    if (f[0] != '_' || f[1] != 'M' || f[2] != 'P' || f[3] != '_' || f[8] == 0 || !sum_ok(f, (uint32_t)f[8] * 16)) return -2;
    o->cpu_count = 0; o->max_apic_id = 0; o->ioapic_count = 0; o->irq_override_count = 0;
    for (int i = 0; i < 16; i++) { o->iso_valid[i] = 0; o->iso_gsi[i] = 0; o->iso_flags[i] = 0; }
    uint32_t cfg = rd32(f + 4);
    if (f[11] != 0 || cfg == 0) {                         /* стандартная конфигурация по номеру: 2 процессора, IOAPIC */
        o->lapic_addr = 0xFEE00000u;
        mp_add_cpu(o, 0); mp_add_cpu(o, 1);
        o->ioapic[0].id = 2; o->ioapic[0].addr = 0xFEC00000u; o->ioapic[0].gsi_base = 0; o->ioapic_count = 1;
        o->valid = 1; o->source = 1;
        return 0;
    }
    if ((uint64_t)cfg > max_phys || max_phys - (uint64_t)cfg < 44) return -3;
    const uint8_t* t = (const uint8_t*)(uintptr_t)cfg;
    uint32_t len = (uint32_t)t[4] | (uint32_t)t[5] << 8;
    if (t[0] != 'P' || t[1] != 'C' || t[2] != 'M' || t[3] != 'P' || len < 44 || len > max_phys - (uint64_t)cfg || !sum_ok(t, len)) return -4;
    o->lapic_addr = rd32(t + 36);
    uint32_t count = (uint32_t)t[34] | (uint32_t)t[35] << 8;
    uint8_t isa[32] = { 0 };                              /* битовая карта номеров шин ISA */
    uint32_t off = 44;
    for (uint32_t n = 0; n < count; n++) {
        if (off >= len) return -4;
        uint8_t type = t[off];
        uint32_t el = type == 0 ? 20 : 8;
        if (type > 4 || el > len - off) return -4;
        const uint8_t* e = t + off;
        if (type == 0 && (e[3] & 1)) mp_add_cpu(o, e[1]);
        else if (type == 1 && e[2] == 'I' && e[3] == 'S' && e[4] == 'A') isa[e[1] >> 3] |= (uint8_t)(1u << (e[1] & 7));
        else if (type == 2 && (e[3] & 1)) {
            if (o->ioapic_count < ACPI_MAX_IOAPIC) {
                o->ioapic[o->ioapic_count].id = e[1];
                o->ioapic[o->ioapic_count].addr = rd32(e + 4);
                o->ioapic[o->ioapic_count].gsi_base = o->ioapic_count * 24;   /* в MP нет базы GSI: по 24 входа на IOAPIC */
            }
            o->ioapic_count++;
        }
        off += el;
    }
    if (off != len) return -4;
    for (unsigned b = 0; b < sizeof isa; b++) if (isa[b]) o->legacy_isa = 1;   /* в таблицах MP есть шина ISA */
    off = 44;                                             /* второй проход: назначения прерываний (шины и IOAPIC уже известны) */
    for (uint32_t n = 0; n < count; n++) {
        if (off >= len) return -4;
        uint8_t type = t[off];
        uint32_t el = type == 0 ? 20 : 8;
        if (type > 4 || el > len - off) return -4;
        const uint8_t* e = t + off;
        if (type == 3 && e[1] == 0) {                     /* INT: шина/IRQ -> IOAPIC/вход */
            uint8_t bus = e[4], irq = e[5], dst = e[6], pin = e[7];
            uint16_t flags = (uint16_t)(e[2] | e[3] << 8);
            if ((isa[bus >> 3] & (1u << (bus & 7))) && irq < 16) {
                uint32_t base = 0; int found = 0;
                for (uint32_t k = 0; k < o->ioapic_count && k < ACPI_MAX_IOAPIC; k++)
                    if (dst == 0xFF || o->ioapic[k].id == dst) { base = o->ioapic[k].gsi_base; found = 1; break; }
                if (found) {
                    o->iso_valid[irq] = 1; o->iso_gsi[irq] = base + pin; o->iso_flags[irq] = flags & 0xF;
                    if (base + pin != irq || (flags & 0xF)) o->irq_override_count++;
                }
            }
        }
        off += el;
    }
    if (off != len) return -4;
    if (!o->cpu_count) return -5;
    o->valid = 1; o->source = 1;
    return 0;
}

int mp_parse(uint64_t fps, uint64_t max_phys, acpi_info_t* out) {
    if (!out) return -1;
    acpi_info_t parsed = *out;
    int rc = mp_parse_impl(fps, max_phys, &parsed);
    if (rc == 0) *out = parsed;
    return rc;
}

static int acpi_finish(uint64_t rs, int from_mb2_tag) {
    const uint64_t ONE_GB = 1ULL << 30;
    info = (acpi_info_t){0};
    if (!rs) rs = acpi_scan(0x80000, 0xA0000, "RSD PTR ", 8, 20);
    if (!rs) rs = acpi_scan(0xE0000, 0x100000, "RSD PTR ", 8, 20);
    int rc = rs ? acpi_parse(rs, ONE_GB, &info) : -2;
    if (rc == 0) {
        uart_print("[KIRA ACPI] rev "); uart_print_dec(info.revision);
        uart_print(from_mb2_tag ? "" : " (RSDP from BootInfo/firmware)");
        uart_print(", tables ok "); uart_print_dec(info.tables_seen);
        uart_print(" bad "); uart_print_dec(info.tables_bad);
        uart_print(" unmapped "); uart_print_dec(info.tables_skipped_high);
        uart_print(", CPUs "); uart_print_dec(info.cpu_count);
        uart_print(", IOAPICs "); uart_print_dec(info.ioapic_count);
        uart_print(", HPET "); uart_print(info.has_hpet ? "yes" : "no");
        uart_print(", PCIe ECAM "); if (info.mcfg_base) uart_print_hex(info.mcfg_base); else uart_print("none");
        uart_print("\n");
        if (info.cpu_count) return 0;
    } else if (rs) {
        uart_print("[KIRA ACPI] RSDP/root table validation failed, code ");
        uart_print_dec((uint64_t)(-rc)); uart_print("\n");
    } else uart_print("[KIRA ACPI] No ACPI tables (BootInfo/Multiboot2/BIOS memory).\n");

    uint64_t fps = acpi_scan(0x80000, 0xA0000, "_MP_", 4, 0);
    if (!fps) fps = acpi_scan(0xE0000, 0x100000, "_MP_", 4, 0);
    acpi_info_t keep = info;
    if (fps && mp_parse(fps, ONE_GB, &info) == 0) {
        if (keep.valid) {
            info.revision = keep.revision; info.has_hpet = keep.has_hpet; info.mcfg_base = keep.mcfg_base;
            info.mcfg_bus_start = keep.mcfg_bus_start; info.mcfg_bus_end = keep.mcfg_bus_end;
            info.tables_seen = keep.tables_seen;
        }
        uart_print("[KIRA ACPI] Using Intel MP tables: CPUs "); uart_print_dec(info.cpu_count);
        uart_print(", IOAPICs "); uart_print_dec(info.ioapic_count);
        uart_print(", ISA IRQ routes "); uart_print_dec(info.irq_override_count); uart_print(" remapped\n");
        return 0;
    }
    info = keep;
    if (!info.valid) { uart_print("[KIRA ACPI] No MP tables either: one processor, PIC.\n"); return -2; }
    return 0;
}

/* Loader-neutral entry: rsdp_phys is a physical address from validated BootInfo.
   acpi_parse maps it through the platform hook when it is outside the early identity map. */
int acpi_init_rsdp(uint64_t rsdp_phys) {
    info = (acpi_info_t){0};
    if (rsdp_phys && (rsdp_phys & 15)) return -1;
    return acpi_finish(rsdp_phys, 0);
}

/* Compatibility entry for the current GRUB/Multiboot2 development path. */
int acpi_init(uint64_t mbi) {
    info = (acpi_info_t){0};
    const uint64_t ONE_GB = 1ULL << 30;
    if (mbi == 0 || mbi >= ONE_GB || (mbi & 7) || mbi > ONE_GB - 16) return -1;
    uint32_t total = rd32((const uint8_t*)(uintptr_t)mbi);
    if (total < 16 || (total & 7) || total > ONE_GB - mbi) return -1;
    const uint8_t* p = (const uint8_t*)(uintptr_t)(mbi + 8);
    const uint8_t* end = (const uint8_t*)(uintptr_t)(mbi + total);
    const uint8_t* rsdp = 0;
    int ended = 0;
    while ((uintptr_t)p <= (uintptr_t)end - 8) {
        uint32_t type = rd32(p), size = rd32(p + 4);
        uint64_t padded = ((uint64_t)size + 7u) & ~7ULL;
        if (size < 8 || size > (uint64_t)(end - p) || padded > (uint64_t)(end - p)) return -1;
        if (type == 0) {
            if (size != 8 || p + padded != end) return -1;
            ended = 1;
            break;
        }
        if (type == 15 && size >= 44) rsdp = p + 8;
        else if (type == 14 && !rsdp && size >= 28) rsdp = p + 8;
        p += padded;
    }
    if (!ended) return -1;
    return acpi_finish((uint64_t)(uintptr_t)rsdp, rsdp != 0);
}

const acpi_info_t* acpi_get(void) { return info.valid ? &info : 0; }
