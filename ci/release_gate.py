#!/usr/bin/env python3
"""Fail-closed release gate for architecture blockers.

A green build is not a release decision. Every numbered engineering area in
RELEASE_GAPS.md must carry an explicit CLOSED marker and repository evidence
files that exist in the exact source tree being packaged.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REQUIRED = set(range(1, 8))
HEADING = re.compile(r"^##\s+(\d+)\.\s+(.+)$")
CLOSED = re.compile(r"^\*\*Gate:\*\* CLOSED\s*$", re.MULTILINE)
EVIDENCE = re.compile(r"^\*\*Evidence:\*\*\s*(.+)$", re.MULTILINE)


def _repo_root(document: Path) -> Path:
    resolved = document.resolve()
    return resolved.parent.parent if resolved.parent.name == "docs" else resolved.parent


def _evidence_errors(raw: str, root: Path) -> list[str]:
    refs = [item.strip() for item in raw.split(";")]
    if not refs or any(not item for item in refs):
        return ["evidence references must be non-empty repository-relative paths separated by ';'"]
    errors = []
    base = root.resolve()
    for ref in refs:
        rel = Path(ref)
        if rel.is_absolute() or any(part in ("", ".", "..") for part in rel.parts):
            errors.append(f"unsafe evidence path: {ref}")
            continue
        candidate = (base / rel).resolve()
        try:
            candidate.relative_to(base)
        except ValueError:
            errors.append(f"evidence path escapes repository: {ref}")
            continue
        if not candidate.is_file():
            errors.append(f"evidence file does not exist: {ref}")
        elif candidate.stat().st_size == 0:
            errors.append(f"evidence file is empty: {ref}")
    return errors


def check(path: Path) -> list[str]:
    document = path.read_text(encoding="utf-8")
    lines = document.splitlines()
    sections: dict[int, tuple[str, list[str]]] = {}
    duplicates: set[int] = set()
    current = None
    for line in lines:
        match = HEADING.match(line)
        if match:
            current = int(match.group(1))
            if current in sections:
                duplicates.add(current)
            sections[current] = (match.group(2), [])
        elif current is not None:
            sections[current][1].append(line)

    errors = []
    for number in sorted(duplicates & REQUIRED):
        errors.append(f"section {number} is duplicated")
    missing = sorted(REQUIRED - sections.keys())
    if missing:
        errors.append("missing required sections: " + ", ".join(map(str, missing)))

    root = _repo_root(path)
    for number in sorted(REQUIRED & sections.keys()):
        title, body_lines = sections[number]
        body = "\n".join(body_lines)
        if not CLOSED.search(body):
            errors.append(f"section {number} ({title}) has no **Gate:** CLOSED marker")
            continue
        evidence = EVIDENCE.search(body)
        if not evidence or not evidence.group(1).strip():
            errors.append(f"section {number} ({title}) has no concrete **Evidence:** reference")
            continue
        for problem in _evidence_errors(evidence.group(1), root):
            errors.append(f"section {number} ({title}): {problem}")
    return errors


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/RELEASE_GAPS.md")
    errors = check(path)
    if errors:
        print("RELEASE BLOCKED: architecture acceptance evidence is incomplete.", file=sys.stderr)
        for error in errors:
            print(" - " + error, file=sys.stderr)
        return 1
    print("RELEASE GATE PASS: all architecture areas are explicitly closed with existing evidence files.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
