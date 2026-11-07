#!/usr/bin/env python3
"""Structural guard for the kernel Mind shared-model synchronization contract."""
from pathlib import Path
import re

source = Path("mind_sys.c").read_text(encoding="utf-8")

def body(signature):
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"Mind sync audit: missing {signature}")
    opening = source.find("{", start)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:i]
    raise SystemExit(f"Mind sync audit: unterminated {signature}")

def without_locked_regions(text):
    return re.sub(r"kmutex_lock\(&mmx\);.*?kmutex_unlock\(&mmx\);", "", text, flags=re.S)

for name, signature in (("mind_sys_tick", "void mind_sys_tick("),
                        ("tact_main", "static void tact_main("),
                        ("mind_sys_admit", "int mind_sys_admit(")):
    outside = without_locked_regions(body(signature))
    if re.search(r"\bM\s*(?:\.|->)|&M\b", outside):
        raise SystemExit(f"Mind sync audit: unguarded model access in {name}")

if re.search(r"\bM\s*(?:\.|->)|&M\b", body("static void publish(")):
    raise SystemExit("Mind sync audit: publish must consume an immutable model snapshot")
if "kmutex_lock(&mmx)" not in body("void mind_sys_save(") or "kmutex_unlock(&mmx)" not in body("void mind_sys_save("):
    raise SystemExit("Mind sync audit: persistence snapshot is not mutex-protected")

for slot in ("req_mode", "req_fx", "req_tz", "req_rtc", "req_cap"):
    if f"mind_sync_take_int(&{slot}," not in body("void mind_sys_tick("):
        raise SystemExit(f"Mind sync audit: {slot} is not atomically consumed")
    if f"mind_sync_store_int(&{slot}," not in source:
        raise SystemExit(f"Mind sync audit: {slot} is not atomically published")

tact = body("static void tact_main(")
if "mind_lever_val(&M, ML_TACT_PERIOD)" not in tact or "kmutex_lock(&mmx)" not in tact:
    raise SystemExit("Mind sync audit: tactical model reads are not protected")

print("Mind sync audit: shared-model paths and request mailboxes satisfy the locking contract")
