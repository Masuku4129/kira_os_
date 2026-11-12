#!/usr/bin/env python3
"""Validate the production KiraFS image inventory before release packaging."""
from __future__ import annotations
import json
import sys
from pathlib import Path

REQUIRED = ("iso_files", "kfs_files", "runtime_library_candidates", "dynamic_dependency_count")
TEST_FIXTURES = {
    "/drv/abad.kdrv", "/drv/abad.rec",
    "/drv/eduun.kdrv", "/drv/eduun.rec",
    "/drv/edu.kdrv", "/drv/edu.rec",
    "/drv/edunc.kdrv", "/drv/edunc.rec",
}


def validate(report: dict) -> list[str]:
    errors = ["missing field: " + k for k in REQUIRED if k not in report]
    if errors:
        return errors
    if not report["iso_files"] or not report["kfs_files"]:
        errors.append("empty ISO or KiraFS inventory")
    if report["runtime_library_candidates"] != 0:
        errors.append("runtime library candidates detected")
    if report["dynamic_dependency_count"] != 0:
        errors.append("ELF dynamic dependencies detected")
    present = sorted(TEST_FIXTURES.intersection(str(p) for p in report["kfs_files"]))
    if present:
        errors.append("unsigned EDU test packages leaked into production image: " + ", ".join(present))
    return errors


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "ci-out/release/image-audit.json")
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"release image audit: cannot read {path}: {exc}", file=sys.stderr)
        return 2
    errors = validate(report)
    if errors:
        print("release image: REJECTED")
        for error in errors:
            print(" - " + error)
        return 1
    if report.get("grub_bundled_in_iso"):
        print("release image: NOTE — GRUB remains in the boot chain; independent loader is still a release blocker")
    print("release image: PASS (no runtime libraries, dynamic dependencies or unsigned EDU fixtures)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
