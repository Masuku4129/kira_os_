#include "boottrust.h"

static boottrust_t g_trust;

void boottrust_init(const kbi_header_t* h) {
    g_trust.flags = 0;
    g_trust.image_version = 0;
    g_trust.reported = 0;
    if (!h) return;
    g_trust.flags = h->trust_flags;
    g_trust.image_version = h->image_version;
    g_trust.reported = (h->boot_mode == KBI_BOOT_UEFI);
}

const boottrust_t* boottrust_get(void) { return &g_trust; }

int boottrust_is_trusted(const boottrust_t* t) {
    const uint32_t need = KBI_TRUST_SB_ENABLED | KBI_TRUST_ENFORCING | KBI_TRUST_SIG_VERIFIED;
    return t && t->reported && (t->flags & need) == need && !(t->flags & KBI_TRUST_SETUP_MODE);
}

int boottrust_report(const boottrust_t* t, void (*out)(const char*), void (*out_dec)(uint64_t)) {
    int lines = 0;
    if (!t || !out) return 0;
    if (!t->reported) {
        out("[KIRA BOOT] Trust chain: the loader reports none (BIOS, GRUB or development boot): kernel signature was not checked\n");
        return 1;
    }
    const uint32_t f = t->flags;
    out("[KIRA BOOT] Secure Boot: ");
    if (!(f & KBI_TRUST_SB_KNOWN)) out("unknown (firmware has no SecureBoot variable)");
    else if (!(f & KBI_TRUST_SB_ENABLED)) out("disabled");
    else if (f & KBI_TRUST_SETUP_MODE) out("enabled, but firmware is in Setup Mode (signatures prove nothing)");
    else out("enabled");
    out("\n");
    lines++;
    if (f & KBI_TRUST_SIG_VERIFIED) {
        out("[KIRA BOOT] Image signature: verified by the loader (boot files match the signed manifest), release ");
        if (out_dec) out_dec(t->image_version);
        out("\n");
    } else {
        out("[KIRA BOOT] Image signature: not checked (development loader)\n");
    }
    lines++;
    out("[KIRA BOOT] Measured boot (TPM PCR 9): ");
    out((f & KBI_TRUST_MEASURED) ? "yes" : "no");
    out((f & KBI_TRUST_CMDLINE_IGNORED) ? "; firmware command line ignored\n" : "\n");
    lines++;
    out(boottrust_is_trusted(t) ? "[KIRA BOOT] Trusted boot: YES\n" : "[KIRA BOOT] Trusted boot: no\n");
    lines++;
    return lines;
}
