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
    return re.sub(r"kmutex_lock\\(&mmx\\);.*?kmutex_unlock\\(&mmx\\);", "", text, flags=re.S)


def require_pattern(text, pattern, message):
    if not re.search(pattern, text, flags=re.S):
        raise SystemExit(f"Mind sync audit: {message}")


model_access = re.compile(r"\\bM\\s*(?:\\.|->)|&M\\b")
for name, signature in (("mind_sys_tick", "void mind_sys_tick("),
                        ("tact_main", "static void tact_main(void* arg) {"),
                        ("mind_sys_admit", "int mind_sys_admit(")):
    outside = without_locked_regions(body(signature))
    if model_access.search(outside):
        raise SystemExit(f"Mind sync audit: unguarded model access in {name}")

if model_access.search(body("static void publish(")):
    raise SystemExit("Mind sync audit: publish must consume an immutable model snapshot")

save = body("void mind_sys_save(")
require_pattern(save,
    r"mind_model_t m;\\s*kmutex_lock\\(&mmx\\);\\s*m\\s*=\\s*M\\.m;\\s*kmutex_unlock\\(&mmx\\);",
    "persistence must copy the model inside the mutex")
tick = body("void mind_sys_tick(")
require_pattern(tick,
    r"kmutex_lock\\(&mmx\\);\\s*mind_step\\(&M,\\s*&S,\\s*&act\\);\\s*published_model\\s*=\\s*M;\\s*kmutex_unlock\\(&mmx\\);",
    "the one-second step and published snapshot must share the model lock")

for slot in ("req_mode", "req_fx", "req_tz", "req_rtc", "req_cap"):
    if f"mind_sync_take_int(&{slot}," not in tick:
        raise SystemExit(f"Mind sync audit: {slot} is not atomically consumed")
    if f"mind_sync_store_int(&{slot}," not in source:
        raise SystemExit(f"Mind sync audit: {slot} is not atomically published")

tact = body("static void tact_main(void* arg) {")
require_pattern(tact,
    r"kmutex_lock\\(&mmx\\);\\s*int32_t period_ms\\s*=\\s*mind_lever_val\\(&M,\\s*ML_TACT_PERIOD\\);\\s*kmutex_unlock\\(&mmx\\);",
    "the tactical period lever read must be inside the model lock")

for name, signature in (("mind_sys_core_cap", "unsigned mind_sys_core_cap("),
                        ("mind_sys_tz", "int32_t mind_sys_tz("),
                        ("mind_sys_rtc_local", "int  mind_sys_rtc_local("),
                        ("mind_sys_local_offset", "int32_t mind_sys_local_offset(")):
    outside = without_locked_regions(body(signature))
    if model_access.search(outside):
        raise SystemExit(f"Mind sync audit: unguarded model accessor {name}")

init = body("void mind_sys_init(")
if "mind_init(&M" not in init or init.find("mind_init(&M") > init.find("ready = 1"):
    raise SystemExit("Mind sync audit: model must be initialized before worker startup")
require_pattern(init,
    r"kmutex_lock\\(&mmx\\);\\s*published_model\\s*=\\s*M;\\s*kmutex_unlock\\(&mmx\\);",
    "initial published snapshot must be copied under the model lock")

view = body("void mind_sys_view(")
require_pattern(view,
    r"spin_lock_irqsave\\(&view_lock\\);\\s*__builtin_memcpy\\(out,\\s*&V,\\s*sizeof V\\);\\s*spin_unlock_irqrestore\\(&view_lock,\\s*f\\);",
    "GUI view reads must use the view lock")
publish = body("static void publish(")
require_pattern(publish,
    r"spin_lock_irqsave\\(&view_lock\\);\\s*__builtin_memcpy\\(&V,\\s*&v,\\s*sizeof V\\);\\s*spin_unlock_irqrestore\\(&view_lock,\\s*f\\);",
    "GUI view publication must use the view lock")

if "__atomic_store_n(&telemetry" not in body("void mind_sys_set_telemetry("):
    raise SystemExit("Mind sync audit: telemetry writes must be atomic")
if "__atomic_load_n(&telemetry" not in body("int  mind_sys_telemetry("):
    raise SystemExit("Mind sync audit: telemetry reads must be atomic")

print("Mind sync audit: model, tactical loop, persistence, GUI snapshots and request mailboxes satisfy the synchronization contract")
