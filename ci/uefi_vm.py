#!/usr/bin/env python3
"""Boot the native x86_64 KIRA UEFI loader under OVMF and verify kernel handoff."""
from __future__ import annotations

import argparse
import glob
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def run(command: list[str], **kwargs) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def find_ovmf_pair() -> tuple[str, str]:
    roots = ["/usr/share/OVMF", "/usr/share/ovmf"]
    codes: list[str] = []
    for root in roots:
        codes.extend(glob.glob(os.path.join(root, "OVMF_CODE*.fd")))
    for code in sorted(codes, key=lambda p: ("4M" not in p, p)):
        name = os.path.basename(code).replace("CODE", "VARS")
        variables = os.path.join(os.path.dirname(code), name)
        if os.path.isfile(variables):
            return code, variables
    raise RuntimeError("matching OVMF_CODE*.fd / OVMF_VARS*.fd pair not found")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--loader", required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--out", default="ci-out/uefi-vm")
    parser.add_argument("--seconds", type=int, default=25)
    args = parser.parse_args()

    for tool in ("qemu-system-x86_64", "mkfs.vfat", "mcopy"):
        if not shutil.which(tool):
            raise RuntimeError(f"required tool missing: {tool}")
    loader = Path(args.loader)
    kernel = Path(args.kernel)
    if not loader.is_file() or not kernel.is_file():
        raise RuntimeError("UEFI loader or kernel payload is missing")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    serial_log = out / "serial.log"
    qemu_log = out / "qemu.log"
    esp = out / "esp.img"
    code, variables_template = find_ovmf_pair()
    variables = out / "OVMF_VARS.fd"
    shutil.copyfile(variables_template, variables)

    with tempfile.TemporaryDirectory(prefix="kira-uefi-") as temp:
        root = Path(temp)
        (root / "EFI" / "BOOT").mkdir(parents=True)
        (root / "EFI" / "KIRA").mkdir(parents=True)
        shutil.copyfile(loader, root / "EFI" / "BOOT" / "BOOTX64.EFI")
        shutil.copyfile(kernel, root / "EFI" / "KIRA" / "KIRA.ELF")
        with esp.open("wb") as image:
            image.truncate(64 * 1024 * 1024)
        run(["mkfs.vfat", "-F", "32", str(esp)])
        run(["mcopy", "-i", str(esp), "-s", str(root / "EFI"), "::/"])

    command = [
        "qemu-system-x86_64", "-machine", "pc,accel=tcg", "-cpu", "qemu64",
        "-m", "2048", "-smp", "2", "-no-reboot", "-no-shutdown",
        "-display", "none", "-monitor", "none",
        "-serial", f"file:{serial_log}",
        "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
        "-drive", f"if=pflash,format=raw,file={variables}",
        "-drive", f"format=raw,file={esp},if=ide",
    ]
    print("+", " ".join(command), flush=True)
    with qemu_log.open("w") as qlog:
        proc = subprocess.Popen(command, stdout=qlog, stderr=subprocess.STDOUT)
        try:
            time.sleep(args.seconds)
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)

    log = serial_log.read_text(errors="replace") if serial_log.exists() else ""
    required = [
        "[KIRA UEFI] Common BootInfo accepted",
        "[KIRA OS] Kernel core starting",
        "[KIRA CONSOLE] Command line ready",
    ]
    missing = [marker for marker in required if marker not in log]
    print("UEFI serial output:")
    print(log[-12000:] if log else "(no serial output)")
    if missing:
        print("UEFI OVMF BOOT FAILED; missing: " + ", ".join(missing), file=sys.stderr)
        print("QEMU log:")
        print(qemu_log.read_text(errors="replace")[-6000:])
        return 1
    print("UEFI OVMF BOOT PASS: firmware loaded BOOTX64.EFI and KIRA reached its command line")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"UEFI OVMF test error: {exc}", file=sys.stderr)
        raise SystemExit(2)
