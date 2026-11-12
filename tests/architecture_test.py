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
x86_main = read("arch/x86_64/kernel_main.c")
x86_boot = read("arch/x86_64/kernel_boot.c")
x86_boot_header = read("arch/x86_64/kernel_boot.h")
kernel_header = read("kernel.h")
require("arch/x86_64/kernel_main.c" in read("build_clang.sh"),
        "x86 kernel entry is not owned by the x86 backend build")
require("arch/x86_64/kernel_boot.c" in read("build_clang.sh"),
        "x86 boot coordinator is not owned by the x86 backend build")
require("kernel_boot_init" not in kernel_header,
        "x86-only boot API leaked into the common kernel header")
require("kernel_boot_init" in x86_boot_header and '#include "kernel_boot.h"' in x86_main,
        "x86 boot coordinator API is not scoped to the x86 backend")
require(not (ROOT / "kernel.c").exists() and not (ROOT / "kernel_boot.c").exists(),
        "architecture-specific kernel entry source remains in the repository root")
require("void kernel_main(" in x86_main and "kernel_boot_init(" in x86_boot,
        "x86 entry/boot ownership is incomplete")
numbers = read("kcc/include/syscall_numbers.h")
for path in ("build_clang.sh", "build_arm64.sh"):
    require("syscall_services.c" in read(path),
            f"{path} does not compile syscall_services.c")

require('#include "syscall_numbers.h"' in core,
        "syscall core does not use the shared syscall-number registry")
require('#include "kcc/include/syscall_numbers.h"' in read("syscall_numbers.h"),
        "kernel forwarding header does not use the canonical registry")
require('#include "../kcc/include/syscall_numbers.h"' in read("userland/kira.h"),
        "userland API does not use the canonical syscall registry")
require('#include "syscall_numbers.h"' in read("kcc/include/kira.h"),
        "KCC public API does not use the canonical syscall registry")
for path in ("userland/kira.h", "kcc/include/kira.h"):
    public_api = read(path)
    duplicated = re.findall(
        r"\bK_(?:FS|HEAP|WRITE|EXIT|TICKS|SLEEP|YIELD|GETPID|GFX_CREATE|GFX_PRESENT|GFX_EVENT|"
        r"THREAD_CREATE|THREAD_EXIT|GETTID|FUTEX_WAIT|FUTEX_WAKE|PORT_OPEN|PORT_SEND|PORT_RECV|"
        r"SHM_OPEN|SHM_MAP|SHM_UNMAP|CLOSE|CLOCK_NS|NET_DNS|NET_CONNECT|NET_SEND|NET_RECV|"
        r"NET_CLOSE|ARGS|NET_LISTEN|NET_ACCEPT|SND_OPEN|SND_WRITE|SND_CLOSE|RANDOM|TIME|"
        r"UDP_OPEN|UDP_SEND|UDP_RECV|UDP_CLOSE|SND_VOLUME|SND_RECORD|NET_CONNECT_SA|"
        r"UDP_SENDTO|UDP_RECVFROM|NET_PEER|JIT)\s*=\s*\d+",
        public_api,
    )
    require(not duplicated, f"{path} duplicates numeric syscall IDs instead of using SYS_* constants")
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
