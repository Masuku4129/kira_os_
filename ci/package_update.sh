#!/usr/bin/env bash
set -euo pipefail
OUT_DIR="${2:-${1:-ci-out/package}}"
RESULTS_DIR="${RESULTS_DIR:-ci-out/results}"
AUDIT_DIR="${AUDIT_DIR:-ci-out/audit}"
KIRA_VERSION="${KIRA_VERSION:-v44}"
BASE_REF="${KIRA_BASE_REF:-origin/v43/integration}"
mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if git rev-parse --verify "$BASE_REF" >/dev/null 2>&1; then
  BASE_SHA="$(git merge-base HEAD "$BASE_REF")"
elif git rev-parse --verify origin/main >/dev/null 2>&1; then
  BASE_SHA="$(git merge-base HEAD origin/main)"
else
  BASE_SHA="$(git rev-parse HEAD)"
fi
HEAD_SHA="$(git rev-parse HEAD)"
RUN_URL="${CI_RUN_URL:-https://github.com/${GITHUB_REPOSITORY:-Masuku4129/kira_os_}/actions/runs/${GITHUB_RUN_ID:-unknown}}"

git archive --format=tar --prefix=kira_os/ HEAD | tar -xf - -C "$STAGE"
R="$STAGE/kira_os/RELEASE"
mkdir -p "$R"
cat > "$R/CHANGELOG.md" <<EOF_CHANGELOG
# KIRA OS $KIRA_VERSION

- Source commit: $HEAD_SHA
- Base commit (v43/integration): $BASE_SHA
- CI run: $RUN_URL

## Реализовано в этом обновлении
- Единая палитра KIRA Aurora для рабочего стола и системных приложений.
- Исправлено перемешивание сообщений Dock на ARM64: каждая запись отправляется в UART атомарно.
- USB Mass Storage по умолчанию монтируется только для чтения; запись, создание и удаление файлов на съёмном томе блокируются.
- GPT: проверяются CRC заголовка и массива записей, допустимые диапазоны разделов и границы LBA.
- Уточнены реальные гарантии проверки драйверов; менеджер драйверов предупреждает, что SHA-256 не является подписью издателя.
- Расширены host-тесты VFS/GPT и выгрузка GUI preview в CI.

## Ограничения, которые остаются
- Криптографическая подпись KDRV-пакетов и управление ключами выпуска ещё не реализованы; текущая SHA-256-сверка не аутентифицирует автора.
- IOMMU/DMA-изоляция зависит от платформы и не гарантируется на всех устройствах.
- Загрузочная цепочка x86 пока использует GRUB/Multiboot2; собственного загрузчика KIRA ещё нет.
- QEMU не заменяет физические испытания x86/ARM устройств.
- Система остаётся независимой: Linux kernel/modules/runtime в образ не включаются.
EOF_CHANGELOG

cat > "$R/INSTALL_TERMUX.md" <<'EOF_INSTALL'
# Установка KIRA OS v44 на Android (Termux/Debian)

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
echo "KIRA OS v44 source archive: verified"
EOF_VERIFY
chmod +x "$R/VERIFY.sh"

python3 - "$RESULTS_DIR" "$R/TEST_REPORT.md" <<'PY'
import json, os, pathlib, sys
root, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
jobs = [("Host tests","HOST_RESULT"),("x86 build","BUILD_RESULT"),("x86 QEMU","VM_RESULT"),("ARM64 build/QEMU","ARM64_RESULT"),("Static audit","AUDIT_RESULT")]
lines = ["# KIRA OS v44 — CI test report","", "Commit: "+os.getenv("GITHUB_SHA","unknown"), "Branch: "+os.getenv("GITHUB_REF_NAME","unknown"), "Run: "+os.getenv("CI_RUN_URL","unknown"), "", "| Job | Result |", "|---|---|"]
lines += ["| "+name+" | "+os.getenv(key,"unknown")+" |" for name,key in jobs]
rows = []
for path in sorted(root.rglob("result.json")):
    try:
        data=json.loads(path.read_text())
        rows.append((data.get("name",path.parent.name),bool(data.get("ok")),data.get("seconds"),data.get("error")))
    except Exception as exc:
        rows.append((path.parent.name,False,None,str(exc)))
lines += ["", "## QEMU scenarios", "", "| Scenario | Result | Seconds | Detail |", "|---|---|---:|---|"]
for name,ok,seconds,error in rows:
    detail=((str(error or "—").splitlines() or ["—"])[0]).replace("|","/")[:160]
    lines.append("| "+str(name)+" | "+("PASS" if ok else "FAIL") + " | "+str(seconds if seconds is not None else "—")+" | "+detail+" |")
host=next(iter(root.rglob("host-tests.log")),None)
lines += ["", "Host log: "+("ALL HOST TESTS PASSED found." if host and "ALL HOST TESTS PASSED" in host.read_text(errors="replace") else "success marker not confirmed in downloaded log."), "", "QEMU is virtual-hardware evidence only; no physical-hardware claim is made.", ""]
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
echo "package: created complete $KIRA_VERSION source archive $OUT_DIR/KIRA_OS_Update.tar.gz"
