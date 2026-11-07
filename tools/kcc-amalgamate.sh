#!/bin/bash
set -euo pipefail
OUT="\${1:-userland/kcc_selfhost.c}"
mkdir -p "$(dirname "$OUT")"
TMP="\${OUT}.tmp"
: > "$TMP"

emit() {
    local file="$1"
    shift
    local spec name
    for spec in "$@"; do printf '#define %s\n' "$spec" >> "$TMP"; done
    cat "$file" >> "$TMP"
    printf '\n' >> "$TMP"
    for spec in "$@"; do name="\${spec%% *}"; printf '#undef %s\n' "$name" >> "$TMP"; done
    printf '\n' >> "$TMP"
}

cat kcc/util.c kcc/lex.c kcc/pp.c kcc/type.c kcc/parse.c \
    kcc/ir.c kcc/gen_common.c >> "$TMP"

# Backend-local static names are prefixed to preserve translation-unit scope
# after the modules are combined into one self-hostable source file.
emit kcc/asm_x86.c "put kcc_x86_asm_put"
emit kcc/gen_x86.c "gen_atomic kcc_x86_gen_atomic"
printf '#undef A\n\n' >> "$TMP"

emit kcc/asm_a64.c \
    "fail kcc_a64_asm_fail" \
    "rd32 kcc_a64_asm_rd32" \
    "wr32 kcc_a64_asm_wr32"

emit kcc/gen_a64.c \
    "ALoc KCC_A64_ALoc" \
    "AL_REG KCC_A64_AL_REG" \
    "AL_STK KCC_A64_AL_STK" \
    "AL_AREG KCC_A64_AL_AREG" \
    "AL_ASTK KCC_A64_AL_ASTK" \
    "xa kcc_a64_xa" \
    "p_ngrn kcc_a64_p_ngrn" \
    "spillbase kcc_a64_spillbase" \
    "plan_args kcc_a64_plan_args" \
    "cmp_start kcc_a64_cmp_start" \
    "spill kcc_a64_spill" \
    "ldv kcc_a64_ldv" \
    "stv kcc_a64_stv" \
    "norm kcc_a64_norm" \
    "copy_bytes kcc_a64_copy_bytes" \
    "ld_reg_bytes kcc_a64_ld_reg_bytes" \
    "gen_bin kcc_a64_gen_bin" \
    "gen_call kcc_a64_gen_call" \
    "gen_atomic kcc_a64_gen_atomic" \
    "gen_asm kcc_a64_gen_asm" \
    "gen_ins kcc_a64_gen_ins" \
    "gen_func kcc_a64_gen_func" \
    "gen_helper kcc_a64_gen_helper"
printf '#undef A\n\n' >> "$TMP"

cat kcc/elf.c >> "$TMP"
printf '\n' >> "$TMP"

# Runtime headers are resolved from /etc/kcc/include/kccrt on the target.
sed \
  -e 's|#include "kccrt/string.h"|#include <string.h>|' \
  -e 's|#include "kccrt/stdlib.h"|#include <stdlib.h>|' \
  -e 's|#include "kccrt/stdio.h"|#include <stdio.h>|' \
  -e 's|#include "kccrt/setjmp.h"|#include <setjmp.h>|' \
  userland/kccrt.c >> "$TMP"
printf '\n' >> "$TMP"
cat userland/libc_min.c >> "$TMP"
printf '\n' >> "$TMP"
sed 's|#include "../kcc/kcc.h"|#include <kcc.h>|' userland/kcc.c >> "$TMP"

mv "$TMP" "$OUT"
