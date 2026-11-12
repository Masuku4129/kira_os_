/* kcc: единая единица трансляции для самосборки (kcc собирает kcc одним файлом: компоновщика отдельных объектов у kcc нет).
   Порядок: ядро компилятора, бэкенд x86-64, затем бэкенд AArch64 с переименованием имён, совпадающих у бэкендов (ren_*.h / un_*.h).
   Запуск: tests/kcc/selfhost.sh (и самосборка в tests/klink_test.sh). Файлы лежат в песочнице плоско (имена без каталогов), поэтому включения без путей. */
#include "util.c"
#include "lex.c"
#include "pp.c"
#include "parse.c"
#include "type.c"
#include "softfp.c"
#include "ir.c"
#include "opt.c"
#include "gen_common.c"
#include "gen_x86.c"
#include "asm_x86.c"
#include "elf.c"
#include "ren_g.h"
#include "gen_a64.c"
#include "un_g.h"
#include "ren_s.h"
#include "asm_a64.c"
#include "un_s.h"
#include "kccrt.c"
/* Режим @build (userland/kcc.c, mind_build.c): те же файлы, что собирает build_clang.sh. В песочнице имя elf.c занято кодом kcc, поэтому
   разбор ELF ядра лежит как rootelf.c; статические имена, совпадающие с именами компилятора, переименованы на время включения. */
#include "rootelf.c"
#include "sha256.c"
#define is_idc mb_is_idc
#define fail mb_fail
#include "mind_build.c"
#undef fail
#undef is_idc
#include "mind_build_core.c"
#include "kcc_main.c"
