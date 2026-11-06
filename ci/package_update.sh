#!/usr/bin/env bash
set -eo pipefail
# Compatible with old call form: package_update.sh origin/main ci-out/package.
OUT_DIR="$2"
if [ -z "$OUT_DIR" ]; then OUT_DIR="$1"; fi
if [ -z "$OUT_DIR" ]; then OUT_DIR=ci-out/package; fi
RESULTS_DIR="$RESULTS_DIR"
if [ -z "$RESULTS_DIR" ]; then RESULTS_DIR=ci-out/results; fi
AUDIT_DIR="$AUDIT_DIR"
if [ -z "$AUDIT_DIR" ]; then AUDIT_DIR=ci-out/audit; fi
mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
if git rev-parse --verify origin/main >/dev/null 2>&1; then
  BASE_SHA="$(git merge-base HEAD origin/main)"
else
  BASE_SHA="$(git rev-parse HEAD)"
fi
HEAD_SHA="$(git rev-parse HEAD)"
RUN_URL="$CI_RUN_URL"
if [ -z "$RUN_URL" ]; then RUN_URL="https://github.com/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID"; fi

# Full source snapshot; the phone-side Debian install does not require a .git directory.
git archive --format=tar --prefix=kira_os/ HEAD | tar -xf - -C "$STAGE"
R="$STAGE/kira_os/RELEASE"
mkdir -p "$R"
cat > "$R/CHANGELOG.md" <<EOF_CHANGELOG
# KIRA OS v43

- Source commit: $HEAD_SHA
- Base commit: $BASE_SHA
- CI run: $RUN_URL

## Integrated
- pkg-DK: Russian application labels, per-app icons, dynamic dock entries, explorer/monitor updates.
- pkg-STAB: adaptive compositor policy and frame-rate limits for background/minimized windows.
- Static dependency/provenance audit and complete source packaging for phone-side installation.

## Release limits
- x86 boot still bundles GRUB/Multiboot2; KIRA's own BIOS/UEFI loader is still absent.
- QEMU tests do not certify physical hardware.
- Read DEPENDENCIES.md and TEST_REPORT.md before production use.
EOF_CHANGELOG
cat > "$R/INSTALL_TERMUX.md" <<'EOF_INSTALL'
# Установка KIRA OS v43 на Android

Архив содержит полный каталог исходников kira_os/ и не требует Git checkout.

1. Сохранить старую папку проекта: mv ~/kira_os ~/kira_os_backup_$(date +%Y%m%d_%H%M%S)
2. Распаковать: tar -xzf /storage/emulated/0/Download/kira_os_v43.tar.gz -C ~
3. Собрать образы: cd ~/kira_os && ./build_clang.sh --android

Свежие kira.iso и kira_disk.img появятся в Download. Старая папка автоматически не удаляется.
EOF_INSTALL
cat > "$R/VERIFY.sh" <<'EOF_VERIFY'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sha256sum -c RELEASE/SHA256SUMS
for f in build_clang.sh build_arm64.sh grub.cfg kernel/linker.ld; do
  test -f "$f" || { echo "missing required file: $f" >&2; exit 1; }
done
echo "KIRA OS v43 source archive: verified"
EOF_VERIFY
chmod +x "$R/VERIFY.sh"

python3 - "$RESULTS_DIR" "$R/TEST_REPORT.md" <<'PY'
import json, os, pathlib, sys
root, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
jobs = [("Host tests","HOST_RESULT"),("x86 build","BUILD_RESULT"),("x86 QEMU","VM_RESULT"),("ARM64 build/QEMU","ARM64_RESULT"),("Static audit","AUDIT_RESULT")]
lines = ["# KIRA OS v43 — CI test report","", "Commit: "+os.getenv("GITHUB_SHA","unknown"), "Branch: "+os.getenv("GITHUB_REF_NAME","unknown"), "Run: "+os.getenv("CI_RUN_URL","unknown"), "", "| Job | Result |", "|---|---|"]
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
    lines.append("| "+str(name)+" | "+("PASS" if ok else "FAIL")+" | "+str(seconds if seconds is not None else "—")+" | "+detail+" |")
host=next(iter(root.rglob("host-tests.log")),None)
lines += ["", "Host log: "+("ALL HOST TESTS PASSED found." if host and "ALL HOST TESTS PASSED" in host.read_text(errors="replace") else "success marker not confirmed in downloaded log."), "", "QEMU is virtual-hardware evidence only; no physical-hardware claim is made.", ""]
dest.write_text("\n".join(lines))
PY

for f in AUDIT.md inventory.json DEPENDENCIES.md dependency_inventory.json; do
  if [ -f "$AUDIT_DIR/$f" ]; then cp "$AUDIT_DIR/$f" "$R/$f"; fi
done
printf '%s\n' "$BASE_SHA" > "$R/BASE_SHA"
printf '%s\n' "$HEAD_SHA" > "$R/HEAD_SHA"
(cd "$STAGE/kira_os" && find . -type f ! -path './RELEASE/SHA256SUMS' -print0 | sort -z | xargs -0 sha256sum > RELEASE/SHA256SUMS)
tar -czf "$OUT_DIR/kira_os_v43.tar.gz" -C "$STAGE" kira_os
(cd "$OUT_DIR" && sha256sum kira_os_v43.tar.gz > kira_os_v43.tar.gz.sha256)
echo "package: created complete source archive $OUT_DIR/kira_os_v43.tar.gz"
