#include "devhealth.h"
#include "spinlock.h"

static spinlock_t lk = SPINLOCK_INIT;
static uint32_t errs, hid_n, hid_rate;
static char err_name[16], hid_name[16];

static void put(char* d, const char* s) { int i = 0; while (s && s[i] && i < 15) { d[i] = s[i]; i++; } d[i] = 0; }
static void get(char* d, const char* s) { for (int i = 0; i < 16; i++) d[i] = s[i]; d[15] = 0; }

void devhealth_error(const char* dev) {
    uint64_t f = spin_lock_irqsave(&lk);
    errs++; put(err_name, dev);
    spin_unlock_irqrestore(&lk, f);
}
void devhealth_take(uint32_t* errors, char name[16]) {
    uint64_t f = spin_lock_irqsave(&lk);
    *errors = errs; errs = 0; get(name, err_name);
    spin_unlock_irqrestore(&lk, f);
}
void devhealth_hid_suspect(const char* dev, uint32_t keys_per_s) {
    uint64_t f = spin_lock_irqsave(&lk);
    hid_n++; if (keys_per_s > hid_rate) hid_rate = keys_per_s; put(hid_name, dev);
    spin_unlock_irqrestore(&lk, f);
}
void devhealth_hid_take(uint32_t* n, uint32_t* rate, char name[16]) {
    uint64_t f = spin_lock_irqsave(&lk);
    *n = hid_n; *rate = hid_rate; hid_n = 0; hid_rate = 0; get(name, hid_name);
    spin_unlock_irqrestore(&lk, f);
}
