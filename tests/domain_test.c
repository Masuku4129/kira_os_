/* Домены приложений (caps.h): потолок прав, квоты, достижимость общих объектов и их применение в kobj. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "caps.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    /* 1. Каждая программа таблицы: права внутри потолка своего домена, домен не «untrusted», если права есть. */
    unsigned n = 0;
    for (const char* nm; (nm = caps_program_at(n)) != 0; n++) {
        uint32_t d = domain_for_program(nm), c = caps_for_program(nm);
        if (c & ~domain_caps_max(d)) { printf("FAIL %s: права 0x%x выше потолка домена %s (0x%x)\n", nm, c, domain_name(d), domain_caps_max(d)); fails++; }
        if (c && d == DOM_UNTRUSTED) { printf("FAIL %s: с правами 0x%x, но в домене untrusted\n", nm, c); fails++; }
        if (!c && d != DOM_UNTRUSTED) { printf("FAIL %s: без прав, но домен %s\n", nm, domain_name(d)); fails++; }
    }
    CHECK(n >= 20);
    /* 2. Чужие и собранные пользователем программы - в untrusted с нулевым потолком. */
    CHECK(domain_for_program("local-x") == DOM_UNTRUSTED && domain_for_program("???") == DOM_UNTRUSTED);
    CHECK(domain_caps_max(DOM_UNTRUSTED) == 0 && domain_caps_max(DOM_COUNT) == 0 && domain_proc_max(DOM_COUNT) == 0);
    CHECK(domain_caps_max(DOM_DRIVER) == CAP_DRIVER_);
    CHECK(!(domain_caps_max(DOM_MEDIA) & (CAP_NET | CAP_DMA | CAP_MMIO)) && !(domain_caps_max(DOM_NET) & (CAP_GFX | CAP_DMA)));
    /* 3. Достижимость: каждый домен - сам себя; system - всех; остальные друг до друга не дотягиваются. */
    for (uint32_t a = 0; a < DOM_COUNT; a++)
        for (uint32_t b = 0; b < DOM_COUNT; b++) {
            int want = a == b || a == DOM_SYSTEM;
            if (domain_may_reach(a, b) != want) { printf("FAIL reach %s -> %s = %d\n", domain_name(a), domain_name(b), domain_may_reach(a, b)); fails++; }
        }
    CHECK(!domain_may_reach(DOM_COUNT, DOM_SYSTEM) && !domain_may_reach(DOM_SYSTEM, DOM_COUNT));
    for (uint32_t d = 0; d < DOM_COUNT; d++) CHECK(domain_proc_max(d) > 0 && strcmp(domain_name(d), "?") != 0);
    printf(fails ? "domain tests: FAILED (%d)\n" : "domain tests: %d failures\n", fails);
    return fails != 0;
}
