#!/usr/bin/env python3
"""Binary-level runtime dependency audit for the built ISO and KiraFS disk."""
import argparse
import json
import re
import struct
import subprocess
import tempfile
from pathlib import Path

LIB_SUFFIXES = {".so", ".dll", ".dylib", ".a", ".ko", ".sys", ".exe"}


def command(args, *, binary=False):
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        err = p.stderr.decode("utf-8", "replace")[-2000:]
        raise RuntimeError(f"{args[0]} exited {p.returncode}: {err}")
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def elf_dependencies(data, label):
    if len(data) < 16 or data[:4] != b"\x7fELF":
        return []
    cls, enc = data[4], data[5]
    if cls not in (1, 2) or enc not in (1, 2):
        raise ValueError(f"{label}: unsupported ELF class/endianness")
    endian = "<" if enc == 1 else ">"
    if cls == 2:
        if len(data) < 64:
            raise ValueError(f"{label}: truncated ELF64 header")
        phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 54)
        phfmt, dynfmt = endian + "IIQQQQQQ", endian + "qQ"
    else:
        if len(data) < 52:
            raise ValueError(f"{label}: truncated ELF32 header")
        phoff = struct.unpack_from(endian + "I", data, 28)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 42)
        phfmt, dynfmt = endian + "IIIIIIII", endian + "iI"
    need = struct.calcsize(phfmt)
    if phentsize < need or phoff + phentsize * phnum > len(data):
        raise ValueError(f"{label}: invalid program-header table")
    found = []
    for i in range(phnum):
        off = phoff + i * phentsize
        h = struct.unpack_from(phfmt, data, off)
        p_type = h[0]
        if cls == 2:
            p_offset, p_filesz = h[2], h[5]
        else:
            p_offset, p_filesz = h[1], h[4]
        if p_type == 3:  # PT_INTERP
            found.append("PT_INTERP (dynamic loader)")
        elif p_type == 2:  # PT_DYNAMIC
            if p_offset + p_filesz > len(data):
                raise ValueError(f"{label}: PT_DYNAMIC outside file")
            step = struct.calcsize(dynfmt)
            for pos in range(p_offset, p_offset + p_filesz, step):
                if pos + step > p_offset + p_filesz:
                    raise ValueError(f"{label}: partial dynamic entry")
                tag, _ = struct.unpack_from(dynfmt, data, pos)
                if tag == 0:
                    break
                if tag == 1:
                    found.append("DT_NEEDED (shared library)")
    return found


def kfs_files(image, tool):
    pending, files = ["/"], []
    seen = set()
    while pending:
        directory = pending.pop()
        if directory in seen:
            raise ValueError(f"KiraFS directory cycle at {directory}")
        seen.add(directory)
        listing = command([tool, image, "ls", directory])
        for line in listing.splitlines():
            m = re.match(r"^([d-])\s+(\d+)\s+(.+)$", line)
            if not m:
                if line.strip():
                    raise ValueError(f"cannot parse kfstool listing: {line!r}")
                continue
            kind, _, name = m.groups()
            if name in (".", "..") or "/" in name:
                raise ValueError(f"invalid KiraFS entry name: {name!r}")
            path = (directory.rstrip("/") + "/" + name) if directory != "/" else "/" + name
            if kind == "d":
                pending.append(path)
            else:
                files.append(path)
    return sorted(files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--kfstool", required=True)
    ap.add_argument("--out", default="ci-out/image-audit.json")
    args = ap.parse_args()
    iso, disk, tool = Path(args.iso), Path(args.disk), Path(args.kfstool)
    if not iso.is_file() or not disk.is_file() or not Path(tool).is_file():
        raise SystemExit("image audit: ISO, disk image or kfstool is missing")

    report = {"iso_files": [], "kfs_files": [], "grub_bootchain_files": [],
              "library_file_candidates": [], "elf_dynamic_dependencies": []}
    with tempfile.TemporaryDirectory(prefix="kira-iso-audit-") as td:
        target = Path(td) / "iso-tree"
        command(["xorriso", "-osirrox", "on", "-indev", str(iso), "-extract", "/", str(target)])
        for p in sorted(x for x in target.rglob("*") if x.is_file()):
            rel = p.relative_to(target).as_posix()
            report["iso_files"].append(rel)
            if rel.startswith("boot/grub/"):
                report["grub_bootchain_files"].append(rel)
            if p.suffix.lower() in LIB_SUFFIXES:
                report["library_file_candidates"].append("ISO:/" + rel)
            report["elf_dynamic_dependencies"].extend(
                f"ISO:/{rel}: {x}" for x in elf_dependencies(p.read_bytes(), "ISO:/" + rel)
            )

    files = kfs_files(str(disk), str(tool))
    for path in files:
        data = command([str(tool), str(disk), "cat", path], binary=True)
        report["kfs_files"].append(path)
        if Path(path).suffix.lower() in LIB_SUFFIXES:
            report["library_file_candidates"].append("KiraFS:" + path)
        report["elf_dynamic_dependencies"].extend(
            f"KiraFS:{path}: {x}" for x in elf_dependencies(data, "KiraFS:" + path)
        )

    report["grub_bundled_in_iso"] = bool(report["grub_bootchain_files"])
    report["runtime_library_candidates"] = len(report["library_file_candidates"])
    report["dynamic_dependency_count"] = len(report["elf_dynamic_dependencies"])
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("image audit: ISO files=%d; KiraFS files=%d; GRUB boot-chain files=%d; library candidates=%d; ELF dynamic dependencies=%d" % (
        len(report["iso_files"]), len(files), len(report["grub_bootchain_files"]),
        report["runtime_library_candidates"], report["dynamic_dependency_count"]))
    if report["runtime_library_candidates"] or report["dynamic_dependency_count"]:
        print("image audit: FAIL - unexpected runtime library/dependency candidate")
        return 1
    print("image audit: PASS - no shared/static kernel or userland libraries and no ELF dynamic dependencies; GRUB remains a separate boot-chain dependency")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
