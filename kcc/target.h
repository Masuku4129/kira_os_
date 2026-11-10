/* kcc: общее для бэкендов (x86-64 и AArch64): таблица имён, достижимость, запись ELF. */
#ifndef KCC_TARGET_H
#define KCC_TARGET_H
#include "kcc.h"

Obj *gen_lookup(const char *name);
int gen_is_defined(const Obj *o);
int gen_is_helper_name(const char *n);       /* memcpy, memset, memcmp, strlen: бэкенд даёт их, если программа не определяет */
Obj *gen_prepare(Obj *globals);
void gen_live(const Ins *i, int idx, int nv, int *vs, int *ve);   /* живые интервалы vreg */

/* elf.c: arch 0 - x86-64 (база 0x8000000000), 1 - AArch64 (база 0x808000000000, как userland/user_arm64.ld).
   code - готовый текст; reloc(text_in_image, ctx) вызывается после назначения адресов (Obj.addr абсолютны) и разрешает ссылки в тексте. */
#define ELF_ARCH_X86 0
#define ELF_ARCH_A64 1
void elf_emit(Obj *globals, const unsigned char *code, size_t codelen, int arch, void (*reloc)(unsigned char *text, void *ctx), void *ctx,
              unsigned char **out, size_t *outlen);
uint64_t elf_base(int arch);
/* Объектный режим (-c): ELF64 ET_REL. text_rel - переносы в тексте (rel32 до символа). */
typedef struct { uint64_t off; Obj *t; int64_t add; int type; } ObjRel;
void elf_emit_obj(Obj *globals, const unsigned char *code, size_t codelen, int arch, const ObjRel *rel, size_t nrel,
                  unsigned char **out, size_t *outlen);
#endif
