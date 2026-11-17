#include "immune.h"
#include "uart.h"

extern char ro_start[], ro_end[], text_start[], text_end[];

// ======================== Иммунная система ==================================

#define MAX_SELF        1024   /* своих программ (по хешу): столько, сколько можно поставить в /bin разумно */
#define MAX_ANTIBODIES  256
#define MAX_QUARANTINE  64
#define ANTIBODY_MAX_LEN 16
#define NOP_SLED_LIMIT  24 // подряд идущих 0x90 (короткие NOP-паддинги нормальны)

typedef struct {
    char name[24];
    uint8_t pattern[ANTIBODY_MAX_LEN];
    uint32_t len;
} antibody_t;

static uint8_t self_digests[MAX_SELF][32];
static uint32_t self_count;
static antibody_t antibodies[MAX_ANTIBODIES];
static uint32_t antibody_total;
static immune_quarantine_entry_t quarantine[MAX_QUARANTINE];
static uint32_t quarantine_total;     // всего записей за время жизни
static uint8_t kernel_baseline[32];
static int baseline_ready;

static void str_copy(char* dst, const char* src, size_t cap) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int digest_equal(const uint8_t* a, const uint8_t* b) {
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static void kernel_digest(uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, ro_start, (size_t)(ro_end - ro_start));
    sha256_update(&c, text_start, (size_t)(text_end - text_start));
    sha256_final(&c, out);
}

int immune_add_antibody(const char* name, const uint8_t* pattern, uint32_t len) {
    if (len == 0 || len > ANTIBODY_MAX_LEN || antibody_total >= MAX_ANTIBODIES) return -1;
    antibody_t* a = &antibodies[antibody_total++];
    str_copy(a->name, name, sizeof(a->name));
    for (uint32_t i = 0; i < len; i++) a->pattern[i] = pattern[i];
    a->len = len;
    return 0;
}

int immune_register_self(const uint8_t digest[32]) {
    if (self_count >= MAX_SELF) return -1;
    for (int i = 0; i < 32; i++) self_digests[self_count][i] = digest[i];
    self_count++;
    return 0;
}

void immune_init(void) {
    kernel_digest(kernel_baseline);
    baseline_ready = 1;
    immune_register_self(kernel_baseline);

    // Встроенные антитела: сигнатуры типичного шелл-кода
    static const uint8_t binsh[] = { '/', 'b', 'i', 'n', '/', 's', 'h' };
    static const uint8_t int80[] = { 0xCD, 0x80, 0x31, 0xC0 };          // int 0x80; xor eax,eax
    static const uint8_t getpc[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x58 }; // call $+5; pop rax
    immune_add_antibody("shellcode:/bin/sh", binsh, sizeof(binsh));
    immune_add_antibody("shellcode:int80", int80, sizeof(int80));
    immune_add_antibody("shellcode:getpc", getpc, sizeof(getpc));

    uart_print("[KIRA IMMUNE] Self baseline taken (SHA-256 of kernel .rodata+.text: ");
    uart_print_dec((uint64_t)((ro_end - ro_start) + (text_end - text_start)));
    uart_print(" bytes, prefix ");
    for (int i = 0; i < 4; i++) {
        static const char hex[] = "0123456789abcdef";
        char b[3] = { hex[kernel_baseline[i] >> 4], hex[kernel_baseline[i] & 15], 0 };
        uart_print(b);
    }
    uart_print("...). ");
    uart_print_dec(antibody_total);
    uart_print(" built-in antibodies + NOP-sled rule (>=24). Quarantine is saved to disk when one is present.\n");
}

int immune_verify_self(void) {
    if (!baseline_ready) return -1;
    uint8_t now[32];
    kernel_digest(now);
    return digest_equal(now, kernel_baseline) ? 0 : -1;
}

static int find_pattern(const uint8_t* buf, size_t size, const uint8_t* pat, uint32_t plen) {
    if (size < plen) return 0;
    for (size_t i = 0; i + plen <= size; i++) {
        uint32_t j = 0;
        while (j < plen && buf[i + j] == pat[j]) j++;
        if (j == plen) return 1;
    }
    return 0;
}

static int has_nop_sled(const uint8_t* buf, size_t size) {
    size_t run = 0;
    for (size_t i = 0; i < size; i++) {
        if (buf[i] == 0x90) {
            if (++run >= NOP_SLED_LIMIT) return 1;
        } else {
            run = 0;
        }
    }
    return 0;
}

static void quarantine_add(const uint8_t digest[32], size_t size, immune_verdict_t v, const char* reason) {
    immune_quarantine_entry_t* e = &quarantine[quarantine_total % MAX_QUARANTINE]; // кольцо
    for (int i = 0; i < 32; i++) e->digest[i] = digest[i];
    e->size = (uint32_t)size;
    e->verdict = v;
    str_copy(e->reason, reason, sizeof(e->reason));
    quarantine_total++;
}

void immune_quarantine_import(const uint8_t digest[32], uint32_t size, uint32_t verdict, const char* reason) {
    if (verdict > IMMUNE_HOSTILE) return;
    quarantine_add(digest, size, (immune_verdict_t)verdict, reason);
}

immune_verdict_t immune_classify(const void* buf, size_t size) {
    if (!buf || size == 0) return IMMUNE_UNKNOWN;
    const uint8_t* p = (const uint8_t*)buf;

    uint8_t digest[32];
    sha256(buf, size, digest);

    // 1. Свой? (точное совпадение дайджеста с базой "своих")
    for (uint32_t i = 0; i < self_count; i++) {
        if (digest_equal(digest, self_digests[i])) return IMMUNE_SELF;
    }

    // 2. Враждебный? (антитела)
    if (has_nop_sled(p, size)) {
        quarantine_add(digest, size, IMMUNE_HOSTILE, "nop-sled");
        return IMMUNE_HOSTILE;
    }
    for (uint32_t i = 0; i < antibody_total; i++) {
        if (find_pattern(p, size, antibodies[i].pattern, antibodies[i].len)) {
            quarantine_add(digest, size, IMMUNE_HOSTILE, antibodies[i].name);
            return IMMUNE_HOSTILE;
        }
    }

    // 3. Чужой и не опознанный: допуска нет, помещается в карантин
    quarantine_add(digest, size, IMMUNE_UNKNOWN, "not-self");
    return IMMUNE_UNKNOWN;
}

void immune_phagocytose(void* buf, size_t size) {
    volatile uint8_t* p = (volatile uint8_t*)buf;
    for (size_t i = 0; i < size; i++) p[i] = 0;
}

uint32_t immune_quarantine_count(void) {
    return quarantine_total < MAX_QUARANTINE ? quarantine_total : MAX_QUARANTINE;
}

const immune_quarantine_entry_t* immune_quarantine_get(uint32_t index) {
    if (index >= immune_quarantine_count()) return 0;
    return &quarantine[index];
}

uint32_t immune_antibody_count(void) {
    return antibody_total;
}
