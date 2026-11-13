#!/usr/bin/env python3
"""Mechanical completeness gate for the PR request-to-plan evidence matrix.

This check validates structure and traceability fields only. It cannot determine
whether the PR truthfully satisfies the user's request; that semantic review is
mandatory in AGENTS.md and docs/PROJECT_CONTROL_CENTER.md.
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

event_path = Path(os.environ.get("GITHUB_EVENT_PATH", ""))
if not event_path.is_file():
    print("FAIL: GITHUB_EVENT_PATH is missing or unreadable")
    sys.exit(1)

try:
    event = json.loads(event_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    print(f"FAIL: cannot read GitHub event payload: {exc}")
    sys.exit(1)

pull_request = event.get("pull_request")
if not isinstance(pull_request, dict):
    print("FAIL: this workflow did not receive a pull_request event")
    sys.exit(1)

body = pull_request.get("body") or ""
required_headings = (
    "## User request and acceptance criteria",
    "## Architecture and dependency impact",
    "## Requirement-to-evidence matrix",
    "## Final compliance review",
)
missing = [heading for heading in required_headings if heading not in body]
if missing:
    for heading in missing:
        print(f"FAIL: missing PR section: {heading}")
    sys.exit(1)

rows = []
for line in body.splitlines():
    stripped = line.strip()
    if stripped.startswith("| R-"):
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        rows.append(cells)

if not rows:
    print("FAIL: no requirement rows found (expected rows beginning with | R-)")
    sys.exit(1)

allowed_statuses = {"PASS", "PARTIAL", "FAIL", "UNVERIFIED", "BLOCKED"}
sha_pattern = re.compile(r"^[0-9a-f]{7,40}$")
errors = []

for index, cells in enumerate(rows, start=1):
    if len(cells) != 7:
        errors.append(f"row {index}: expected 7 columns, found {len(cells)}")
        continue
    requirement_id, user_req, plan, component, sha, evidence, status = cells
    if not re.fullmatch(r"R-[0-9]{2,}", requirement_id):
        errors.append(f"row {index}: invalid requirement ID {requirement_id!r}")
    for label, value in (
        ("user requirement", user_req),
        ("master plan mapping", plan),
        ("component/test", component),
        ("evidence", evidence),
    ):
        if not value or value in {"...", "TBD", "TODO", "-"}:
            errors.append(f"row {index}: missing {label}")
    if not sha_pattern.fullmatch(sha.strip("`")):
        errors.append(f"row {index}: exact SHA must be 7-40 lowercase hex characters")
    normalized_status = status.split("(", 1)[0].strip()
    if normalized_status not in allowed_statuses:
        errors.append(f"row {index}: invalid status {status!r}")

if errors:
    for error in errors:
        print(f"FAIL: {error}")
    sys.exit(1)

print(
    f"PASS: PR compliance form has {len(rows)} populated requirement row(s). "
    "This is a structural check only; semantic request/plan review remains mandatory."
)
