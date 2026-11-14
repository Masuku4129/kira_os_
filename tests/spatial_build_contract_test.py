#!/usr/bin/env python3
"""Ensure CI and both architecture builders actually compile Spatial Minimal."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
x86 = (root / "build_clang.sh").read_text(encoding="utf-8")
arm = (root / "build_arm64.sh").read_text(encoding="utf-8")
ci = (root / ".github/workflows/ci.yml").read_text(encoding="utf-8")

checks = {
    "x86 builder exposes --spatial-preview": '--spatial-preview' in x86 and 'KIRA_SPATIAL_PREVIEW=1' in x86,
    "x86 builder accepts --no-run in any argument position": '*" --no-run "*) exit 0 ;;' in x86,
    "ARM64 builder accepts KIRA_SPATIAL_PREVIEW=1": 'KIRA_SPATIAL_PREVIEW:-0' in arm and '-DKIRA_SPATIAL_PREVIEW=1' in arm,
    "x86 CI image is Spatial Minimal": './build_clang.sh --no-run --spatial-preview' in ci,
    "ARM64 CI image is Spatial Minimal": 'KIRA_SPATIAL_PREVIEW=1 ./build_arm64.sh' in ci,
    "Android/Limbo build enables Spatial Minimal": '*" --android "*' in x86 and '-DKIRA_SPATIAL_PREVIEW=1' in x86,
    "Android/Limbo output dispatch matches both flags": '*" --android-full "*|*" --android "*)' in x86,
    "Android/Limbo package stages and verifies disk/ISO before atomic replacement": all(token in x86 for token in ('cp -p kira.iso "$ISO_TMP"', 'cp -p "$OUT/kira_disk.img" "$DISK_TMP"', 'tools/kfstool "$DISK_TMP" ls / >/dev/null', 'mv -f "$DISK_TMP" "$OUT/kira_disk.img"', 'mv -f "$ISO_TMP" "$OUT/kira.iso"', 'KIRA-FULL-SHA256SUMS')) and 'tools/kfstool "$OUT/kira_disk.img" put' not in x86,
}
failed = [name for name, ok in checks.items() if not ok]
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
if failed:
    raise SystemExit(1)
print("build: Spatial Minimal is explicitly enabled in both CI architecture jobs")
