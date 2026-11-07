#!/usr/bin/env python3
"""Fail-closed release gate for architecture blockers.

A green build is not a release decision. Every numbered engineering area in
RELEASE_GAPS.md must carry an explicit CLOSED marker and concrete evidence.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REQUIRED = set(range(1, 8))
HEADING = re.compile(r"^##\s+(\d+)\.\s+(.+)$")
CLOSED = re.compile(r"^\*\*Gate:\*\* CLOSED\s*$", re.MULTILINE)
EVIDENCE = re.compile(r"^\*\*Evidence:\*\*\s*(.+)$", re.MULTILINE)


def check(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    sections: dict[int, tuple[str, list[str]]] = {}
    current = None
    for line in lines:
        match = HEADING.match(line)
        if match:
            current = int(match.group(1))
            sections[current] = (match.group(2), [])
        elif current is not None:
            sections[current][1].append(line)

    errors = []
    missing = sorted(REQUIRED - sections.keys())
    if missing:
        errors.append("missing required sections: " + ", ".join(map(str, missing)))
    for number in sorted(REQUIRED & sections.keys()):
        title, body_lines = sections[number]
        body = "\n".join(body_lines)
        if not CLOSED.search(body):
            errors.append(f"section {number} ({title}) has no **Gate:** CLOSED marker")
            continue
        evidence = EVIDENCE.search(body)
        if not evidence or not evidence.group(1).strip():
            errors.append(f"section {number} ({title}) has no concrete **Evidence:** reference")
    return errors


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/RELEASE_GAPS.md")
    errors = check(path)
    if errors:
        print("RELEASE BLOCKED: architecture acceptance evidence is incomplete.", file=sys.stderr)
        for error in errors:
            print(" - " + error, file=sys.stderr)
        return 1
    print("RELEASE GATE PASS: all architecture areas are explicitly closed with evidence.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
