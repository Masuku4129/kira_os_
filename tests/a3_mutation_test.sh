#!/bin/bash
# Мутационная проверка защиты от отката и подмены (А3): в копию исходника вносится одна порча (убрана проверка), и соответствующий хост-тест ОБЯЗАН упасть.
# Если порча проходит незамеченной, тест ничего не проверяет. Каждая порча сначала проверяется на то, что нужный текст в исходнике есть (иначе ложный успех).
set -u
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
BAD=0; N=0
# mutate ФАЙЛ СТАРОЕ НОВОЕ ТЕСТ: тест - tpm2 | rollback | kfs | appman
mutate() {
    local f=$1 old=$2 new=$3 t=$4
    N=$((N+1))
    rm -rf $T/m; mkdir -p $T/m; cp tpm2.c tpm2.h rollback.c rollback.h kfs.c kfs.h appman.c appman.h kdrv_sig.c kdrv_state.c $T/m/
    python3 - "$T/m/$f" "$old" "$new" <<'PY' || { echo "ОШИБКА СТЕНДА: в $f нет текста порчи: $old"; BAD=$((BAD+1)); return; }
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
if s.count(old) != 1: sys.exit(1)
open(p, "w").write(s.replace(old, new))
PY
    local ok=1
    case $t in
      tpm2)     $CC -O1 -w -I$T/m -Itests -o $T/m/t tests/tpm2_test.c $T/m/tpm2.c 2>/dev/null && $T/m/t >/dev/null 2>&1 && ok=0 ;;
      rollback) $CC -O1 -w -I$T/m -Itests -o $T/m/t tests/rollback_test.c $T/m/rollback.c $T/m/tpm2.c 2>/dev/null && $T/m/t >/dev/null 2>&1 && ok=0 ;;
      kfs)      $CC -O1 -w -DKIRA_HOST_TEST -I$T/m -I. -Iarch/x86_64 -o $T/m/t tests/kfs_epoch_test.c $T/m/kfs.c $T/m/rollback.c $T/m/tpm2.c sha256.c zstd.c store.c 2>/dev/null && $T/m/t >/dev/null 2>&1 && ok=0 ;;
      appman)   A3_SRC_DIR=$T/m CC="$CC -w" ASAN_OPTIONS=detect_leaks=0 bash tests/appman_test.sh >/dev/null 2>&1 && ok=0 ;;
    esac
    if [ $ok = 0 ]; then echo "НЕ ЗАМЕЧЕНА порча [$t] $f: $old"; BAD=$((BAD+1)); else echo "замечена  [$t] $f: ${old:0:70}"; fi
}
mutate rollback.c 'if ((uint64_t)image_version < in->counter) return RB_E_ROLLBACK_TPM;' 'if (0) return RB_E_ROLLBACK_TPM;' rollback
mutate rollback.c 'if (in->static_min && image_version < in->static_min) return RB_E_ROLLBACK_MIN;' 'if (0) return RB_E_ROLLBACK_MIN;' rollback
mutate rollback.c 'if (in->require_tpm) return RB_E_NO_TPM;' 'if (0) return RB_E_NO_TPM;' rollback
mutate rollback.c 'return gap > RB_MAX_RAISE ? RB_MAX_RAISE : (uint32_t)gap;' 'return (uint32_t)gap;' rollback
mutate rollback.c 'uint64_t gap = (uint64_t)image_version - counter;' 'uint64_t gap = (uint64_t)image_version - counter + 1;' rollback
mutate rollback.c 'return disk_epoch < anchor ? RB_DISK_ROLLBACK : RB_DISK_OK;' 'return RB_DISK_OK;' rollback
mutate rollback.c 'return disk_epoch < anchor ? RB_DISK_ROLLBACK : RB_DISK_OK;' 'return disk_epoch <= anchor ? RB_DISK_ROLLBACK : RB_DISK_OK;' rollback
mutate rollback.c 'return persisted_epoch > anchor;' 'return 1;' rollback
mutate rollback.c 'if (e == TPM2_E_EMPTY) { value = 0; e = TPM2_OK; }' 'if (0) { value = 0; e = TPM2_OK; }' rollback
mutate rollback.c 'if (in[4] || in[5] || in[6] || in[7] != 1) return RB_E_ARGS;' '' rollback
mutate tpm2.c 'if (size != len) return TPM2_E_FORMAT;' '' tpm2
mutate tpm2.c 'if (*rc != 0 && len != 10) return TPM2_E_FORMAT;' '' tpm2
mutate tpm2.c 'if (e == 0x0E || e == 0x09) return TPM2_E_AUTH;' '' tpm2
mutate tpm2.c 'if (psize != 2u + dsize || dsize != want) return TPM2_E_FORMAT;' 'if (dsize != want) return TPM2_E_FORMAT;' tpm2
mutate kfs.c '#define KFS_MUTATE(call) do { KFS_LOCK(); int r_ = fs.ro ? KFS_ERR_RO : (call); if (r_ == 0) fs.dirty = 1;' '#define KFS_MUTATE(call) do { KFS_LOCK(); int r_ = (call); if (r_ == 0) fs.dirty = 1;' kfs
mutate kfs.c 'if (r_ == 0) fs.dirty = 1;' 'if (r_ == 0) fs.dirty = 0;' kfs
mutate kfs.c 'if (r) fs.epoch--; else fs.dirty = 0;' 'if (r) fs.epoch--; else fs.dirty = 1;' kfs
mutate kfs.c 'sb->epoch_lo = (uint32_t)fs.epoch; sb->epoch_hi = (uint32_t)(fs.epoch >> 32);' '' kfs
mutate appman.c 'return diff ? TRUST_TAMPERED : TRUST_OK;' 'return TRUST_OK;' appman
mutate appman.c 'return (trusted || dev_flag) ? APPMAN_SIGNED : APPMAN_DEV;' 'return dev_flag ? APPMAN_SIGNED : APPMAN_DEV;' appman
mutate appman.c 'if (rd32(blob + 16) != 0) return APPMAN_E_FORMAT;' '' appman
mutate appman.c 'if (same((const char*)e, (const char*)f)) return APPMAN_E_FORMAT;' '' appman
mutate kdrv_sig.c 'if (found->flags & KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_REVOKED;' '' appman
mutate kdrv_sig.c 'if (found->not_after && (uint64_t)now_utc >= found->not_after) return KDRV_SIG_E_KEY_EXPIRED;' '' appman
mutate kdrv_sig.c "const uint8_t KDRV_SIG_DOMAIN_APPS[16]   = { 'K','I','R','A','-','A','P','P','S','-','S','I','G','-','V','1' };" "const uint8_t KDRV_SIG_DOMAIN_APPS[16]   = { 'K','I','R','A','-','A','P','P','-','S','I','G','-','-','V','1' };" appman
mutate kdrv_sig.c 'if (!equal_bytes(digest, s->package_sha256, sizeof digest)) return KDRV_SIG_E_HASH;' '' appman
mutate kdrv_state.c 'if (kdrv_state_is_revoked(st, keys[i].id)) eff_keys[i].flags |= KDRV_SIG_KEY_REVOKED;' '' appman
echo "порч проверено: $N, не замечено или ошибок стенда: $BAD"
[ $BAD = 0 ]
