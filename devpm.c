/* Реестр хуков сна и пробуждения: см. devpm.h. */
#include "devpm.h"

typedef struct { const char* name; devpm_fn suspend, resume; void* ctx; int state; int last_rc; } hook_t;
static hook_t H[DEVPM_MAX];
static int N;
static void (*logf)(const char*);
static const char* refused_name;
static int refused_rc;

void devpm_set_log(void (*fn)(const char*)) { logf = fn; }

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void put_int(char* o, unsigned* n, unsigned cap, int v) {
    char t[12]; unsigned k = 0; unsigned u = v < 0 ? (unsigned)(-(int64_t)v) : (unsigned)v;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u && k < 11);
    if (v < 0 && *n + 1 < cap) o[(*n)++] = '-';
    while (k && *n + 1 < cap) o[(*n)++] = t[--k];
    o[*n] = 0;
}
static void put_str(char* o, unsigned* n, unsigned cap, const char* s) { while (*s && *n + 1 < cap) o[(*n)++] = *s++; o[*n] = 0; }

/* "[KIRA SLEEP] <имя>: <текст> (код N)" */
static void say(const char* name, const char* text, int rc, int with_rc) {
    if (!logf) return;
    char b[160]; unsigned n = 0; b[0] = 0;
    put_str(b, &n, sizeof b, "[KIRA SLEEP] "); put_str(b, &n, sizeof b, name); put_str(b, &n, sizeof b, ": "); put_str(b, &n, sizeof b, text);
    if (with_rc) { put_str(b, &n, sizeof b, " (код "); put_int(b, &n, sizeof b, rc); put_str(b, &n, sizeof b, ")"); }
    logf(b);
}

int devpm_register(const char* name, devpm_fn suspend, devpm_fn resume, void* ctx) {
    if (!name || !*name || (!suspend && !resume)) return -1;
    for (int i = 0; i < N; i++) if (same(H[i].name, name)) {
        H[i].suspend = suspend; H[i].resume = resume; H[i].ctx = ctx; H[i].state = 0; H[i].last_rc = 0;
        return 0;
    }
    if (N >= DEVPM_MAX) return -1;
    H[N].name = name; H[N].suspend = suspend; H[N].resume = resume; H[N].ctx = ctx; H[N].state = 0; H[N].last_rc = 0;
    N++;
    return 0;
}
int devpm_count(void) { return N; }

static int do_resume(int i) {
    hook_t* h = &H[i];
    if (h->state != 1) return 0;
    int rc = h->resume ? h->resume(h->ctx) : 0;
    h->last_rc = rc;
    if (rc < 0) { h->state = 2; say(h->name, "не восстановилось после сна, устройство отключено", rc, 1); return 1; }
    h->state = 0;
    return 0;
}

int devpm_suspend_all(void) {
    refused_name = 0; refused_rc = 0;
    for (int i = N - 1; i >= 0; i--) {
        hook_t* h = &H[i];
        h->last_rc = 0;
        int rc = h->suspend ? h->suspend(h->ctx) : 0;
        h->last_rc = rc;
        if (rc < 0) {
            refused_name = h->name; refused_rc = rc;
            say(h->name, "устройство не переживёт сон, сон отменён", rc, 1);
            for (int k = i + 1; k < N; k++) (void)do_resume(k);      /* откат: уже усыплённые - обратно, в порядке регистрации */
            return -1;
        }
        h->state = 1;
    }
    return 0;
}

int devpm_resume_all(void) {
    int bad = 0;
    for (int i = 0; i < N; i++) bad += do_resume(i);
    return bad;
}

const char* devpm_refusal(void) { return refused_name; }
int devpm_refusal_code(void) { return refused_rc; }
int devpm_info(int i, devpm_info_t* out) {
    if (i < 0 || i >= N) return -1;
    out->name = H[i].name; out->state = H[i].state; out->last_rc = H[i].last_rc;
    return 0;
}
