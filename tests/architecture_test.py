#!/usr/bin/env python3
"""Static architecture guard for the transitional syscall/service split."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

def read(path):
    return (ROOT / path).read_text(encoding="utf-8")

def require(condition, message):
    if not condition:
        print("FAIL:", message)
        sys.exit(1)

core = read("syscall.c")
adapter = read("syscall_services.c")
numbers = read("syscall_numbers.h")
for path in ("build_clang.sh", "build_arm64.sh"):
    require("syscall_services.c" in read(path),
            f"{path} does not compile syscall_services.c")

require('#include "syscall_numbers.h"' in core,
        "syscall core does not use the shared syscall-number registry")
require('#include "syscall_services.h"' in core,
        "syscall core does not use the service-adapter contract")
for header in ("net.h", "audio.h", "gfxsrv.h", "fsrv.h", "rtc.h"):
    require(f'#include "{header}"' not in core,
            f"service header {header} leaked back into syscall core")

core_defines = re.findall(r"^\s*#\s*define\s+(SYS_[A-Z0-9_]+)\b", core, re.M)
require(not core_defines,
        "syscall numbers must have one source of truth; found defines in syscall.c")

ids = re.findall(r"^\s*#\s*define\s+(SYS_[A-Z0-9_]+)\s+(\d+)\b", numbers, re.M)
names = [name for name, _ in ids]
values = [value for _, value in ids]
require(len(names) == len(set(names)), "duplicate syscall names in syscall_numbers.h")
require(len(values) == len(set(values)), "duplicate syscall numeric IDs in syscall_numbers.h")

service_ids = [
    "SYS_GFX_CREATE", "SYS_GFX_PRESENT", "SYS_GFX_EVENT", "SYS_FS",
    "SYS_NET_DNS", "SYS_NET_CONNECT", "SYS_NET_SEND", "SYS_NET_RECV",
    "SYS_NET_CLOSE", "SYS_NET_LISTEN", "SYS_NET_ACCEPT",
    "SYS_SND_OPEN", "SYS_SND_WRITE", "SYS_SND_CLOSE", "SYS_SND_VOLUME",
    "SYS_SND_RECORD", "SYS_UDP_OPEN", "SYS_UDP_SEND", "SYS_UDP_RECV",
    "SYS_UDP_CLOSE", "SYS_NET_CONNECT_SA", "SYS_NET_PEER",
    "SYS_UDP_SENDTO", "SYS_UDP_RECVFROM", "SYS_TIME",
]
for name in service_ids:
    require(name in names, f"{name} missing from the shared syscall registry")
    require(re.search(r"\bcase\s+" + name + r"\b", adapter) is not None,
            f"{name} missing from service adapter dispatch")

require("SYS_ARGS" not in adapter,
        "process argument syscall must remain in the kernel core")
require("SYS_JIT" not in adapter,
        "process JIT syscall must remain in the kernel core")
require("syscall_service_dispatch(nr, a1, a2, a3, &service_result)" in core,
        "kernel core does not route unknown service IDs through the adapter")

print("PASS: syscall numbers are centralized; core/service boundary and both ISA build manifests are consistent")
