#!/usr/bin/env bash
set -euo pipefail
OUT_DIR="${2:-${1:-ci-out/package}}"
RESULTS_DIR="${RESULTS_DIR:-ci-out/results}"
AUDIT_DIR="${AUDIT_DIR:-ci-out/audit}"
KIRA_VERSION="${KIRA_VERSION:-v45}"
BASE_REF="${KIRA_BASE_REF:-edae4ab3367a055826f431e261af5d6dee790ddf}"
mkdir -p "$OUT_DIR"
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

for key in HOST_RESULT BUILD_RESULT VM_RESULT ARM64_RESULT AUDIT_RESULT MIND_RESULT GUI_RESULT KCC_RESULT DRIVER_RESULT; do
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
HOST_LOG="$(find_required_log host-tests.log)"
MIND_LOG="$(find_required_log mind-quality.log)"
GUI_LOG="$(find_required_log gui-regression.log)"
KCC_LOG="$(find_required_log kcc-tests.log)"
DRIVER_LOG="$(find_required_log driver-e2e.log)"

git archive --format=tar --prefix=kira_os/ "$HEAD_SHA" | tar -xf - -C "$STAGE"
R="$STAGE/kira_os/RELEASE"
mkdir -p "$R"

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
# Установка KIRA OS v45 на Android (Termux/Debian)

Архив содержит полный каталог исходников `kira_os/`; это архив исходников, а не готовый ISO.

1. Сохранить предыдущий проект:
   `mv ~/kira_os ~/kira_os_backup_$(date +%Y%m%d_%H%M%S)`
2. Распаковать архив из Download:
   `tar -xzf /storage/emulated/0/Download/KIRA_OS_Update.tar.gz -C ~`
3. Перейти в проект и проверить контрольные суммы:
   `cd ~/kira_os && bash RELEASE/VERIFY.sh`
4. Собрать и установить образы в Download:
   `./build_clang.sh --android`

Скрипт сборки перезаписывает `Download/kira.iso`. Если `Download/kira_disk.img` уже существует, он обновляет системные программы на нём и сохраняет пользовательские данные и опыт; если файла нет — создаёт новый. Для Limbo: CD-ROM = `kira.iso`, Hard Disk A = `kira_disk.img`, Boot from = CD-ROM.

Не удаляйте резервную папку проекта, пока не проверите новую сборку в Limbo.
EOF_INSTALL
cat > "$R/VERIFY.sh" <<'EOF_VERIFY'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sha256sum -c RELEASE/SHA256SUMS
for f in build_clang.sh build_arm64.sh grub.cfg arch/x86_64/linker.ld arch/aarch64/linker.ld drivers/index.txt ci/dependency_audit.py docs/DEPENDENCIES.md docs/V44_UPGRADE_PLAN.md docs/V44_SECURITY_MODEL.md; do
  test -f "$f" || { echo "missing required file: $f" >&2; exit 1; }
done
echo "KIRA OS v45 source archive: verified"
EOF_VERIFY
chmod +x "$R/VERIFY.sh"

python3 - "$RESULTS_DIR" "$R/TEST_REPORT.md" "$HOST_LOG" "$MIND_LOG" "$GUI_LOG" "$KCC_LOG" "$DRIVER_LOG" <<'PY'
import json, os, pathlib, sys
root, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
host_log, mind_log, gui_log, kcc_log, driver_log = map(pathlib.Path, sys.argv[3:8])
jobs = [
    ("Host tests", "HOST_RESULT"), ("x86_64 build", "BUILD_RESULT"),
    ("x86_64 QEMU matrix", "VM_RESULT"), ("ARM64 build and QEMU", "ARM64_RESULT"),
    ("Static dependency/source audit", "AUDIT_RESULT"),
    ("Mind quality (ASan/UBSan)", "MIND_RESULT"), ("GUI regression", "GUI_RESULT"),
    ("KCC compiler suite", "KCC_RESULT"), ("Driver installer E2E", "DRIVER_RESULT"),
]
lines = ["# KIRA OS v45 — CI test report", "",
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
          "- Host tests: " + str(host_log.relative_to(root)),
          "- Mind quality: " + str(mind_log.relative_to(root)),
          "- GUI regression: " + str(gui_log.relative_to(root)),
          "- KCC compiler suite: " + str(kcc_log.relative_to(root)),
          "- Driver installer E2E: " + str(driver_log.relative_to(root)), "",
          "## Scope limits", "",
          "- QEMU is virtual-hardware evidence only; physical x86_64/ARM64 hardware remains untested.",
          "- A successful static audit is not proof of memory safety or complete dependency absence.",
          "- SHA-256 checksums do not authenticate the publisher.", ""]
dest.write_text("\n".join(lines))
PY

for f in AUDIT.md inventory.json DEPENDENCIES.md dependency_inventory.json; do
  if [ -f "$AUDIT_DIR/$f" ]; then cp "$AUDIT_DIR/$f" "$R/$f"; fi
done
printf '%s\n' "$BASE_SHA" > "$R/BASE_SHA"
printf '%s\n' "$HEAD_SHA" > "$R/HEAD_SHA"
printf '%s\n' "$KIRA_VERSION" > "$R/VERSION"
(cd "$STAGE/kira_os" && find . -type f ! -path './RELEASE/SHA256SUMS' -print0 | sort -z | xargs -0 sha256sum > RELEASE/SHA256SUMS)
tar -czf "$OUT_DIR/KIRA_OS_Update.tar.gz" -C "$STAGE" kira_os
(cd "$OUT_DIR" && sha256sum KIRA_OS_Update.tar.gz > KIRA_OS_Update.tar.gz.sha256)
echo "package: created verified $KIRA_VERSION source archive $OUT_DIR/KIRA_OS_Update.tar.gz"
