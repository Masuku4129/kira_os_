#!/bin/sh
# Reproducible two-stage KCC bootstrap. Stage 1 is built by the host compiler;
# stage 2 is compiled by stage 1 and then linked with the same target linker.
# This is a bootstrap verification tool, not a claim that KCC is already
# self-hosted on bare metal.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"
HOST_CC=${HOST_CC:-clang}
HOST_LD=${HOST_LD:-ld.lld}
TARGET=${TARGET:-x86_64}
OUT=${OUT:-"$ROOT/out/kcc-bootstrap"}
mkdir -p "$OUT/stage1" "$OUT/stage2"
case "$TARGET" in x86_64|aarch64) ;; *) echo "unsupported target: $TARGET" >&2; exit 2;; esac

CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter"
SOURCES=""
for f in kcc/*.c; do
    [ "$f" = kcc/main.c ] && continue
    SOURCES="$SOURCES $f"
done

# Build stage 1 with the trusted host compiler.
$HOST_CC $CFLAGS -Ikcc -c kcc/main.c -o "$OUT/stage1/main.o"
for f in $SOURCES; do
    b=$(basename "$f" .c)
    $HOST_CC $CFLAGS -Ikcc -c "$f" -o "$OUT/stage1/$b.o"
done
$HOST_CC -o "$OUT/stage1/kcc" "$OUT/stage1/"*.o

# Stage 1 compiles the compiler translation units. The project compiler emits
# Kira ELF, so use the same target ABI and include paths as ordinary KCC builds.
for f in kcc/*.c; do
    b=$(basename "$f" .c)
    "$OUT/stage1/kcc" -target "$TARGET" -I kcc -I kcc/include -o "$OUT/stage2/$b.elf" "$f"
done

printf '%s\n' "KCC stage 1 built: $OUT/stage1/kcc"
printf '%s\n' "KCC stage 2 objects emitted for $TARGET: $OUT/stage2"
printf '%s\n' "NOTE: stage-2 execution and byte-for-byte stage-3 reproducibility require the Kira ELF loader."
