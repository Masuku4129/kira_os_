#!/usr/bin/env bash
set -euo pipefail
OUT_DIR="${2:-${1:-ci-out/package}}"
RESULTS_DIR="${RESULTS_DIR:-ci-out/results}"
AUDIT_DIR="${AUDIT_DIR:-ci-out/audit}"
IMAGE_AUDIT_DIR="${IMAGE_AUDIT_DIR:-ci-out/release-image-audit}"
IMAGE_AUDIT="${IMAGE_AUDIT_DIR}/image-audit.json"
RELEASE_IMAGE_DIR="${RELEASE_IMAGE_DIR:-ci-out/release-image}"
ARM64_IMAGE_DIR="${ARM64_IMAGE_DIR:-ci-out/arm64-image}"
UEFI_IMAGE_DIR="${UEFI_IMAGE_DIR:-ci-out/uefi/EFI}"
UEFI_LOG_DIR="${UEFI_LOG_DIR:-ci-out/uefi-ovmf}"
KIRA_VERSION="${KIRA_VERSION:-v46}"
BASE_REF="${KIRA_BASE_REF:-edae4ab3367a055826f431e261af5d6dee790ddf}"
mkdir -p "$OUT_DIR"
# Never publish a combined update while architecture blockers remain open.
# KIRA_PREVIEW=1 builds a clearly labelled test archive and never replaces the release.
if [ "${KIRA_PREVIEW:-0}" = "1" ]; then
  echo "package: PREVIEW mode - release gate NOT satisfied, archive is a test build"
  python3 ci/release_gate.py docs/RELEASE_GAPS.md || true
else
  python3 ci/release_gate.py docs/RELEASE_GAPS.md
fi
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if ! git rev-parse --verify "${BASE_REF}^{commit}" >/dev/null 2>&1; then
  echo "ERROR: required v43 base commit is unavailable: $BASE_REF" >&2
  exit 1
fi
BASE_SHA="$(git merge-base HEAD "$BASE_REF")"
if [ "$BASE_SHA" != "$BASE_REF" ]; then
  echo "ERROR: HEAD is not based on the declared v43 base; merge-base=$BASE_SHA expected=$BASE_REF" >&2
  exit 1
fi
HEAD_SHA="$(git rev-parse HEAD)"
RUN_URL="${CI_RUN_URL:-https://github.com/${GITHUB_REPOSITORY:-Masuku4129/kira_os_}/actions/runs/${GITHUB_RUN_ID:-unknown}}"

for key in HOST_RESULT BUILD_RESULT VM_RESULT ARM64_RESULT AUDIT_RESULT MIND_RESULT GUI_RESULT KCC_RESULT DRIVER_RESULT UEFI_RESULT IMAGE_AUDIT_RESULT; do
  value="${!key:-}"
  if [ "$value" != "success" ]; then
    echo "ERROR: required CI job $key is not successful (value: ${value:-missing})" >&2
    exit 1
  fi
done

find_required_log() {
  local name="$1" found
  found="$(find "$RESULTS_DIR" -type f -name "$name" -print -quit 2>/dev/null || true)"
  if [ -z "$found" ]; then
    echo "ERROR: required CI evidence log is missing: $name" >&2
    exit 1
  fi
  printf '%s\n' "$found"
}
if [ ! -s "$IMAGE_AUDIT" ]; then
  echo "ERROR: required binary ISO/KiraFS audit report is missing: $IMAGE_AUDIT" >&2
  exit 1
fi
python3 - "$IMAGE_AUDIT" <<'PY_AUDIT'
import json, pathlib, sys
report = json.loads(pathlib.Path(sys.argv[1]).read_text())
required = ("iso_files", "kfs_files", "runtime_library_candidates", "dynamic_dependency_count")
missing = [key for key in required if key not in report]
if missing:
    raise SystemExit("image audit report missing fields: " + ", ".join(missing))
if not report["iso_files"] or not report["kfs_files"]:
    raise SystemExit("image audit report has empty ISO or KiraFS inventory")
if report["runtime_library_candidates"] != 0 or report["dynamic_dependency_count"] != 0:
    raise SystemExit("image audit detected runtime library or ELF dynamic dependency candidates")
print("package: binary image audit evidence verified")
PY_AUDIT
python3 ci/release_image.py "$IMAGE_AUDIT"

# The release archive must carry the exact images produced and audited by this CI run.
for image in "$RELEASE_IMAGE_DIR/kira.iso" "$RELEASE_IMAGE_DIR/kira_disk.img" \
             "$ARM64_IMAGE_DIR/kira-arm64.img" "$ARM64_IMAGE_DIR/kira-arm64.disk" \
             "$UEFI_IMAGE_DIR/BOOT/BOOTX64.EFI" "$UEFI_IMAGE_DIR/KIRA/KIRA.ELF"; do
  if [ ! -s "$image" ]; then
    echo "ERROR: tested release image is missing or empty: $image" >&2
    exit 1
  fi
done

HOST_LOG="$(find_required_log host-tests.log)"
MIND_LOG="$(find_required_log mind-quality.log)"
GUI_LOG="$(find_required_log gui-regression.log)"
KCC_LOG="$(find_required_log kcc-tests.log)"
DRIVER_LOG="$(find_required_log driver-e2e.log)"
UEFI_LOG="$UEFI_LOG_DIR/serial.log"
if [ ! -s "$UEFI_LOG" ]; then echo "ERROR: successful OVMF boot serial evidence is missing: $UEFI_LOG" >&2; exit 1; fi

git archive --format=tar --prefix=kira_os/ "$HEAD_SHA" | tar -xf - -C "$STAGE"
R="$STAGE/kira_os/RELEASE"
mkdir -p "$R"
cp "$RELEASE_IMAGE_DIR/kira.iso" "$R/kira.iso"
cp "$RELEASE_IMAGE_DIR/kira_disk.img" "$R/kira_disk.img"
cp "$ARM64_IMAGE_DIR/kira-arm64.img" "$R/kira-arm64.img"
cp "$ARM64_IMAGE_DIR/kira-arm64.disk" "$R/kira-arm64.disk"
mkdir -p "$R/uefi/EFI"
cp -R "$UEFI_IMAGE_DIR/BOOT" "$R/uefi/EFI/"
cp -R "$UEFI_IMAGE_DIR/KIRA" "$R/uefi/EFI/"
cp "$UEFI_LOG" "$R/uefi/ovmf-serial.log"

{
  echo "# KIRA OS $KIRA_VERSION"
  echo
  echo "- Source commit: $HEAD_SHA"
  echo "- Base commit: $BASE_SHA"
  echo "- CI run: $RUN_URL"
  echo
  echo "## Изменения относительно v43"
  echo
  git log --no-merges --reverse --format='- %s (%h)' "$BASE_SHA..$HEAD_SHA"
  echo
  echo "## Границы подтверждённого результата"
  echo
  echo "- Состав изменений выше сформирован из фактической истории Git, а не из заранее заданного списка."
  echo "- Успешный QEMU не является подтверждением работы на физическом оборудовании."
  echo "- SHA-256 контрольная сумма подтверждает целостность файла, но не подлинность издателя."
  echo "- Linux kernel/modules/runtime в состав ОС не включаются."
} > "$R/CHANGELOG.md"

cat > "$R/INSTALL_TERMUX.md" <<'EOF_INSTALL'
# Установка KIRA OS v46 на Android (Termux/Debian)

Архив содержит исходники и **готовые образы, собранные и проверенные тем же CI run**:
- `RELEASE/kira.iso` — готовый x86-64 ISO;
- `RELEASE/kira_disk.img` — системный диск, собранный тем же CI run;
- `RELEASE/kira-arm64.img` и `RELEASE/kira-arm64.disk` — ARM64 артефакты, если они были собраны.

1. Распаковать архив из Download:
   `mkdir -p ~/kira_os && tar -xzf /storage/emulated/0/Download/KIRA_OS_Update.tar.gz -C ~`
2. Перейти в проект и проверить контрольные суммы:
   `cd ~/kira_os && bash RELEASE/VERIFY.sh`
3. Установить готовый ISO в Android Download:
   `cp -f RELEASE/kira.iso /storage/emulated/0/Download/kira.iso`
4. Если системного диска ещё нет, создать его из поставленного образа:
   `test -e /storage/emulated/0/Download/kira_disk.img || cp RELEASE/kira_disk.img /storage/emulated/0/Download/kira_disk.img`

**Важно:** существующий `kira_disk.img` не перезаписывается этой инструкцией, чтобы не потерять пользовательские файлы и опыт Mind. Для обновления системных файлов на уже существующем диске после резервного копирования используйте из каталога проекта `./build_clang.sh --android`; этот путь собирает образ и обновляет существующий диск по логике build script.

Для Limbo: CD-ROM = `kira.iso`, Hard Disk A = `kira_disk.img`, Boot from = CD-ROM. Перед проверкой убедитесь, что Limbo использует именно скопированный ISO.

SHA-256 архива проверяйте командой `sha256sum -c KIRA_OS_Update.tar.gz.sha256` из каталога Download.
EOF_INSTALL
if [ "${KIRA_PREVIEW:-0}" = "1" ]; then
  sed -i 's/KIRA_OS_Update\.tar\.gz/KIRA_OS_PREVIEW.tar.gz/g; 1a\
\
**ТЕСТОВАЯ СБОРКА, не релиз** (см. PREVIEW_NOT_RELEASE.txt).' "$R/INSTALL_TERMUX.md"
fi
cat > "$R/VERIFY.sh" <<'EOF_VERIFY'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sha256sum -c RELEASE/SHA256SUMS
for f in build_clang.sh build_arm64.sh grub.cfg arch/x86_64/linker.ld arch/aarch64/linker.ld drivers/index.txt ci/dependency_audit.py docs/DEPENDENCIES.md docs/DRIVERS.md docs/KCC.md docs/V44_UPGRADE_PLAN.md docs/V44_SECURITY_MODEL.md; do
  test -f "$f" || { echo "missing required file: $f" >&2; exit 1; }
done
echo "KIRA OS v46 source archive: verified"
EOF_VERIFY
chmod +x "$R/VERIFY.sh"

python3 - "$RESULTS_DIR" "$R/TEST_REPORT.md" "$HOST_LOG" "$MIND_LOG" "$GUI_LOG" "$KCC_LOG" "$DRIVER_LOG" "$UEFI_LOG" <<'PY'
import json, os, pathlib, sys
root, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
host_log, mind_log, gui_log, kcc_log, driver_log, uefi_log = map(pathlib.Path, sys.argv[3:9])
jobs = [
    ("Host tests", "HOST_RESULT"), ("x86_64 build", "BUILD_RESULT"),
    ("x86_64 QEMU matrix", "VM_RESULT"), ("ARM64 build and QEMU", "ARM64_RESULT"),
    ("Static dependency/source audit", "AUDIT_RESULT"),
    ("Mind quality (ASan/UBSan)", "MIND_RESULT"), ("GUI regression", "GUI_RESULT"),
    ("KCC compiler suite", "KCC_RESULT"), ("Driver installer E2E", "DRIVER_RESULT"), ("UEFI OVMF boot", "UEFI_RESULT"),
    ("Binary ISO/KiraFS audit", "IMAGE_AUDIT_RESULT"),
]
lines = ["# KIRA OS v46 — CI test report", "",
         "Commit: " + os.getenv("GITHUB_SHA", "unknown"),
         "Branch: " + os.getenv("GITHUB_REF_NAME", "unknown"),
         "Run: " + os.getenv("CI_RUN_URL", "unknown"), "",
         "| Required job | Result |", "|---|---|"]
lines += ["| " + name + " | " + os.getenv(key, "missing") + " |" for name, key in jobs]
results = []
for path in sorted(root.rglob("result.json")):
    try:
        data = json.loads(path.read_text())
        results.append((str(data.get("name", path.parent.name)), bool(data.get("ok")),
                        data.get("seconds"), data.get("error"), str(path.relative_to(root))))
    except Exception as exc:
        results.append((path.parent.name, False, None, str(exc), str(path.relative_to(root))))
lines += ["", "## QEMU scenario evidence", "",
          "| Scenario | Result | Seconds | Evidence | Detail |",
          "|---|---|---:|---|---|"]
for name, ok, seconds, error, evidence in results:
    detail = ((str(error or "—").splitlines() or ["—"])[0]).replace("|", "/")[:160]
    lines.append("| " + name.replace("|", "/") + " | " + ("PASS" if ok else "FAIL") +
                 " | " + str(seconds if seconds is not None else "—") + " | " +
                 evidence.replace("|", "/") + " | " + detail + " |")
arm = sum(1 for _,_,_,_,path in results if "arm64" in path.lower())
lines += ["", "Result JSON files: " + str(len(results)) + "; ARM64 result files: " + str(arm) + ".", "",
          "## Captured logs", "",
          "- Host tests: " + os.path.relpath(host_log, root),
          "- Mind quality: " + os.path.relpath(mind_log, root),
          "- GUI regression: " + os.path.relpath(gui_log, root),
          "- KCC compiler suite: " + os.path.relpath(kcc_log, root),
          "- Driver installer E2E: " + os.path.relpath(driver_log, root),
          "- UEFI OVMF serial: " + os.path.relpath(uefi_log, root), "",
          "## Scope limits", "",
          "- QEMU is virtual-hardware evidence only; physical x86_64/ARM64 hardware remains untested.",
          "- A successful static audit is not proof of memory safety or complete dependency absence.",
          "- SHA-256 checksums do not authenticate the publisher.", ""]
dest.write_text("\n".join(lines))
PY

cp "$IMAGE_AUDIT" "$R/IMAGE_AUDIT.json"
for f in AUDIT.md inventory.json DEPENDENCIES.md dependency_inventory.json; do
  if [ -f "$AUDIT_DIR/$f" ]; then cp "$AUDIT_DIR/$f" "$R/$f"; fi
done
if [ "${KIRA_PREVIEW:-0}" = "1" ]; then
  cat > "$R/PREVIEW_NOT_RELEASE.txt" <<'EOF_PREVIEW'
ТЕСТОВАЯ СБОРКА, НЕ РЕЛИЗ.
Все CI-проверки зелёные (host, build, QEMU-матрица, UEFI/OVMF, ARM64, KCC, GUI, Mind, driver E2E),
но релизный шлюз docs/RELEASE_GAPS.md не закрыт: BIOS-загрузка, самосборка KCC, RTL8139 clean-room,
подписи драйверов, DMA, платформа, GUI/Mind остаются открытыми. Проверено только в QEMU/OVMF,
на физическом железе не проверялось.
EOF_PREVIEW
  KIRA_ARCHIVE=KIRA_OS_PREVIEW.tar.gz
fi
printf '%s\n' "$BASE_SHA" > "$R/BASE_SHA"
printf '%s\n' "$HEAD_SHA" > "$R/HEAD_SHA"
printf '%s\n' "$KIRA_VERSION" > "$R/VERSION"
(cd "$STAGE/kira_os" && find . -type f ! -path './RELEASE/SHA256SUMS' -print0 | sort -z | xargs -0 sha256sum > RELEASE/SHA256SUMS)
ARCHIVE="${KIRA_ARCHIVE:-KIRA_OS_Update.tar.gz}"
tar -czf "$OUT_DIR/$ARCHIVE" -C "$STAGE" kira_os
(cd "$OUT_DIR" && sha256sum "$ARCHIVE" > "$ARCHIVE.sha256")
echo "package: created $KIRA_VERSION archive (source + tested images) $OUT_DIR/$ARCHIVE"
