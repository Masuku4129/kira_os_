/* ARM64: общая часть враждебных программ (аналоги userland/atk-*.s для x86-64). */
#include "kira.h"
static void say(const char* s) { k_write(s, k_strlen(s)); }
