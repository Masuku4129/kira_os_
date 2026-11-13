#!/usr/bin/env python3
"""Fail CI when a tracked source disappears or a new file is not classified."""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATUS_DIR = ROOT / "docs" / "PROJECT_STATUS"


def fail(message: str) -> None:
    print("FAIL: " + message)
    sys.exit(1)


def words_from(pattern: str, source: str, label: str) -> list[str]:
    match = re.search(pattern, source, re.MULTILINE)
    if not match:
        fail("cannot read " + label + " from build script")
    return match.group(1).strip().split()


manifest = json.loads((STATUS_DIR / "status_manifest.json").read_text(encoding="utf-8"))
inventory = json.loads((STATUS_DIR / "branch_inventory_2026-11-13.json").read_text(encoding="utf-8"))

# 1. Every file in the pinned main baseline must still exist.
baseline = set(inventory["baseline"]["files"])
missing = sorted(path for path in baseline if not (ROOT / path).is_file())
if missing:
    fail("baseline files disappeared: " + ", ".join(missing[:30]))

# 2. Every tracked path must be accounted for by the baseline or this registry.
managed = set(manifest["managed_files"])
try:
    raw = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
except (OSError, subprocess.CalledProcessError) as exc:
    fail("git ls-files unavailable: " + str(exc))
tracked = {p.decode("utf-8") for p in raw.split(b"\0") if p}
unclassified = sorted(tracked - baseline - managed)
stale = sorted((baseline | managed) - tracked)
if unclassified:
    fail("new tracked files are not classified in status_manifest: " + ", ".join(unclassified[:30]))
if stale:
    fail("manifest expects paths absent from Git index: " + ", ".join(stale[:30]))

# 3. All declared baseline/build inputs must exist.
ready = manifest["ready"]
required = set(ready["required_files"])
required.update(ready["x86_64_kernel_sources"])
required.update(ready["x86_64_arch_sources"])
required.update(ready["arm64_kernel_sources"])
for path in sorted(required):
    if not (ROOT / path).is_file():
        fail("required baseline/build input is missing: " + path)

# 4. Keep the release-input lists synchronized with the actual build scripts.
x86_script = (ROOT / "build_clang.sh").read_text(encoding="utf-8")
arm_script = (ROOT / "build_arm64.sh").read_text(encoding="utf-8")
x86_sources = words_from(r'^C_SOURCES="([^"]+)"$', x86_script, "C_SOURCES")
x86_arch = [
    item.replace("$ARCH/", "arch/x86_64/")
    for item in words_from(r'^ARCH_SOURCES="([^"]+)"$', x86_script, "ARCH_SOURCES")
]
arm_match = re.search(r"# Общее ядро[\s\S]*?for f in([\s\S]*?); do", arm_script)
if not arm_match:
    fail("cannot locate ARM64 common-kernel source list")
arm_sources = [
    item.replace("$A/", "arch/aarch64/")
    for item in arm_match.group(1).strip().split()
    if item != "\\"
]

def app_names(source: str) -> list[str]:
    found: set[str] = set()
    for match in re.finditer(r"for app in ([^;]+); do", source):
        found.update(match.group(1).strip().split())
    return sorted(found)

checks = [
    ("x86_64_kernel_sources", x86_sources),
    ("x86_64_arch_sources", x86_arch),
    ("arm64_kernel_sources", arm_sources),
    ("x86_userland_app_names", app_names(x86_script)),
    ("arm64_userland_app_names", app_names(arm_script)),
]
for key, actual in checks:
    expected = ready[key]
    if actual != expected:
        fail(key + " differs from build script; update the manifest after reviewing the build change")

# Userland programs are selected by name in shell loops; verify their source files too.
for target, names in (
    ("x86-64", ready["x86_userland_app_names"]),
    ("ARM64", ready["arm64_userland_app_names"]),
):
    for name in names:
        if not (ROOT / "userland" / (name + ".c")).is_file() and not (ROOT / "userland" / (name + ".s")).is_file():
            fail(target + " userland source is missing for app " + name)

# 5. The branch-only inventory must remain pinned and internally complete.
expected_diffs = {
    "v46_not_in_main": 437,
    "main_not_in_v46": 21,
    "autodrv_not_in_v46": 56,
    "architecture_not_in_main": 50,
    "spatial_not_in_main": 10,
    "spatial_tokens_not_in_spatial_base": 2,
    "release_not_in_spatial": 0,
}
for key, count in expected_diffs.items():
    actual = inventory["differences"].get(key)
    if actual is None or len(actual) != count:
        fail("branch snapshot count changed for " + key + "; refresh the snapshot deliberately")
if len(inventory["open_pull_requests"]) != 58 or len(inventory["closed_unmerged_pull_requests"]) != 49:
    fail("PR snapshot changed; refresh branch/PR inventory deliberately")

for pr in inventory["open_pull_requests"] + inventory["closed_unmerged_pull_requests"]:
    if not pr.get("head") or not pr.get("head_sha") or not pr.get("base"):
        fail("PR snapshot contains an incomplete head/base record")

print("PASS: all pinned baseline files still exist")
print("PASS: every tracked file is classified by the three-stage registry")
print("PASS: x86-64 and ARM64 build inputs match the manifest")
print("PASS: branch-only files and PR snapshots remain pinned")
