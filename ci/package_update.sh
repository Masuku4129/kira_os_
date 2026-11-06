#!/usr/bin/env bash
set -euo pipefail

BASE_REF="${1:-origin/main}"
OUT_DIR="${2:-ci-out/package}"
RESULTS_DIR="${RESULTS_DIR:-ci-out/results}"
AUDIT_DIR="${AUDIT_DIR:-ci-out/audit}"

git fetch -q origin main --depth=1
BASE_SHA="$(git rev-parse "$BASE_REF")"
HEAD_SHA="$(git rev-parse HEAD)"
mkdir -p "$OUT_DIR"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

git diff --binary "$BASE_SHA...$HEAD_SHA" > "$STAGE/update.patch"
if [[ ! -s "$STAGE/update.patch" ]]; then
  echo "package: no changes relative to $BASE_REF; nothing to package"
  exit 0
fi

printf '%s\n' "$BASE_SHA" > "$STAGE/BASE_SHA"
printf '%s\n' "$HEAD_SHA" > "$STAGE/HEAD_SHA"

cat > "$STAGE/README.md" <<'EOF'
# KIRA OS update package

This is a source patch for an existing KIRA OS Git checkout. It does not contain a prebuilt ISO.

1. Extract this archive.
2. From Debian/Termux, run: bash install.sh /path/to/kira_os
3. Rebuild locally from the project directory: ./build_clang.sh --android
4. To reverse this patch before making further changes: bash rollback.sh /path/to/kira_os

The installer checks the base commit, working-tree cleanliness, patch applicability and SHA-256 checksums before changing files. It does not run a build or overwrite the Limbo disk image.
EOF

cat > "$STAGE/install.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-$PWD}"
TARGET="$(cd "$TARGET" && pwd)"
if [[ ! -f "$TARGET/build_clang.sh" ]]; then
  echo "install: target must be the KIRA OS project root (build_clang.sh not found)" >&2
  exit 2
fi
if ! git -C "$TARGET" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "install: target must be a Git checkout; no files were changed" >&2
  exit 2
fi
BASE_SHA="$(cat "$HERE/BASE_SHA")"
if ! git -C "$TARGET" merge-base --is-ancestor "$BASE_SHA" HEAD; then
  echo "install: this checkout does not contain package base $BASE_SHA; update/reconcile the project first" >&2
  exit 2
fi
if ! git -C "$TARGET" diff --quiet || ! git -C "$TARGET" diff --cached --quiet; then
  echo "install: working tree is not clean; commit or back up local changes first" >&2
  exit 2
fi
(cd "$HERE" && sha256sum -c SHA256SUMS)
git -C "$TARGET" apply --check "$HERE/update.patch"
git -C "$TARGET" apply "$HERE/update.patch"
git -C "$TARGET" diff --check
echo "KIRA OS source patch applied."
echo "Next: cd '$TARGET' && ./build_clang.sh --android"
EOF

cat > "$STAGE/rollback.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-$PWD}"
TARGET="$(cd "$TARGET" && pwd)"
if ! git -C "$TARGET" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "rollback: target must be a Git checkout" >&2
  exit 2
fi
git -C "$TARGET" apply --reverse --check "$HERE/update.patch"
git -C "$TARGET" apply --reverse "$HERE/update.patch"
git -C "$TARGET" diff --check
echo "KIRA OS patch reversed."
EOF
chmod +x "$STAGE/install.sh" "$STAGE/rollback.sh"

python3 - "$RESULTS_DIR" "$STAGE/TEST_REPORT.md" <<'PY'
import json
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
dest = pathlib.Path(sys.argv[2])
statuses = {
    "host-tests": os.getenv("HOST_RESULT", "unknown"),
    "x86_64 build": os.getenv("BUILD_RESULT", "unknown"),
    "x86_64 QEMU matrix": os.getenv("VM_RESULT", "unknown"),
    "ARM64 build/QEMU": os.getenv("ARM64_RESULT", "unknown"),
    "static audit": os.getenv("AUDIT_RESULT", "unknown"),
}
lines = [
    "# KIRA OS — CI test report",
    "",
    f"- Commit: {os.getenv('GITHUB_SHA', 'unknown')}",
    f"- Branch: {os.getenv('GITHUB_REF_NAME', 'unknown')}",
    f"- Workflow run: {os.getenv('CI_RUN_URL', 'not supplied')}",
    "",
    "## Job results",
    "",
    "| Job | Result |",
    "|---|---|",
]
lines.extend(f"| {name} | {value} |" for name, value in statuses.items())
lines += ["", "## QEMU scenario results", ""]
results = []
for path in sorted(root.rglob("result.json")):
    try:
        data = json.loads(path.read_text())
        results.append((data.get("name", path.parent.name), bool(data.get("ok")), data.get("seconds"), data.get("error")))
    except Exception as exc:
        results.append((path.parent.name, False, None, f"cannot parse result.json: {exc}"))
if results:
    lines += ["| Scenario | Result | Duration | Detail |", "|---|---|---:|---|"]
    for name, ok, seconds, error in results:
        detail = (str(error or "").splitlines()[0]).replace("|", "/")[:180]
        lines.append(f"| {name} | {'PASS' if ok else 'FAIL'} | {seconds if seconds is not None else '—'} | {detail} |")
else:
    lines.append("No result.json files were downloaded; scenario-level status is unavailable.")
host_logs = list(root.rglob("host-tests.log"))
if host_logs:
    log = host_logs[0].read_text(errors="replace")
    lines += ["", "## Host test log", "", "- " + ("ALL HOST TESTS PASSED found." if "ALL HOST TESTS PASSED" in log else "Final host-test success marker not found; inspect the attached CI log.")]
else:
    lines += ["", "Host test log artifact was not available in this package job."]
lines += [
    "",
    "QEMU validates virtual hardware only. This report does not claim physical-hardware validation.",
    "",
]
dest.write_text("\n".join(lines))
PY

if [[ -f "$AUDIT_DIR/AUDIT.md" ]]; then
  cp "$AUDIT_DIR/AUDIT.md" "$STAGE/AUDIT.md"
else
  printf '%s\n' '# KIRA OS audit' '' 'Audit artifact was not available; inspect the workflow run.' > "$STAGE/AUDIT.md"
fi

(
  cd "$STAGE"
  sha256sum README.md BASE_SHA HEAD_SHA update.patch install.sh rollback.sh TEST_REPORT.md AUDIT.md > SHA256SUMS
)
tar -czf "$OUT_DIR/KIRA_OS_Update.tar.gz" -C "$STAGE" .
(
  cd "$OUT_DIR"
  sha256sum KIRA_OS_Update.tar.gz > KIRA_OS_Update.tar.gz.sha256
)
echo "package: created $OUT_DIR/KIRA_OS_Update.tar.gz"
