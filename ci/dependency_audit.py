#!/usr/bin/env python3
"""Static dependency and source-provenance inventory for KIRA OS."""
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_EXT = {".c", ".h", ".s", ".S", ".inc"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
TOOLS = {
    "clang": "build-time compiler",
    "ld.lld": "build-time linker",
    "nasm": "build-time x86 assembler",
    "wasm-ld": "build-time WebAssembly linker",
    "llvm-objcopy": "build-time ARM image converter",
    "grub-mkrescue": "build-time ISO tool; GRUB itself is included in the ISO",
    "xorriso": "build-time ISO authoring tool",
    "qemu-system-x86_64": "test-only virtual x86 machine",
    "qemu-system-aarch64": "test-only virtual ARM machine",
    "qemu-system-arm": "test-only virtual ARM machine",
    "gcc": "host-side build/test compiler",
    "python3": "CI/test orchestration",
    "openssl": "test oracle, not an OS library",
    "python3-cryptography": "test oracle, not an OS library",
    "python3-pil": "test image helper, not an OS library",
    "dosfstools": "host-side FAT test utility",
    "mtools": "host-side FAT image utility",
    "ca-certificates": "trust-root data copied to the system disk",
}
GENERATED = {"manifest.h"}


def main():
    raw = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=True).stdout
    files = sorted(x.decode("utf-8", "replace") for x in raw.split(b"\0") if x)
    sources = [ROOT / p for p in files if Path(p).suffix in SOURCE_EXT and not p.startswith("tests/")]
    build_paths = {
        "build_clang.sh", "build_arm64.sh", "run_qemu.sh",
        "userland/wasm/build.sh", ".github/workflows/ci.yml",
    }
    build_text = "\n".join((ROOT / p).read_text(errors="replace") for p in files if p in build_paths)
    include_hits, provenance = [], []
    search_roots = ["arch/x86_64", "arch/aarch64", "userland", "kcc/include", "userland/kccrt", "include", "tests"]
    for path in sources:
        content = path.read_text(errors="replace")
        rel = str(path.relative_to(ROOT))
        for kind, name in INCLUDE.findall(content):
            candidates = [path.parent / name, ROOT / name] + [ROOT / base / name for base in search_roots]
            if not any(p.is_file() for p in candidates) and Path(name).name not in GENERATED:
                include_hits.append({"file": rel, "header": name, "kind": "local-unresolved" if kind == '"' else "compiler-header"})
        for n, line in enumerate(content.splitlines(), 1):
            if re.search(r"FreeBSD|OpenBSD|Linux kernel|SPDX-License-Identifier:\\s*BSD", line, re.I):
                provenance.append({"file": rel, "line": n, "text": line.strip()[:180]})

    link_lines = "\n".join(line for line in build_text.splitlines() if re.search(r"(clang|gcc|ld\\.lld|wasm-ld|\\$CC|\\$HOSTCC)", line))
    link_flags = sorted(set(re.findall(r"(?:^|\\s)-l([A-Za-z0-9_+.-]+)", link_lines)))
    lib_ext = {".so", ".dll", ".dylib", ".a", ".ko", ".sys", ".exe"}
    lib_files = [p for p in files if Path(p).suffix.lower() in lib_ext]
    vendor_dirs = sorted({part for p in files for part in Path(p).parts[:-1]
                          if part.lower() in {"vendor", "vendors", "third_party", "third-party", "external", "externals"}})
    grub = "grub-mkrescue" in build_text and (ROOT / "grub.cfg").exists()
    grub = grub and "multiboot2" in (ROOT / "grub.cfg").read_text(errors="replace")
    ca_bundle = "/etc/ssl/roots.der" in build_text and "ca-certificates" in build_text
    no_libc_evidence = "-nostdlib" in build_text and not link_flags
    detected = [{"tool": name, "role": role} for name, role in TOOLS.items()
                if re.search(r"(?<![A-Za-z0-9_-])" + re.escape(name) + r"(?![A-Za-z0-9_-])", build_text, re.I)]
    actions = sorted(set(re.findall(r"^\\s*-\\s*uses:\\s*([^\\s#]+)", build_text, re.M)))
    report = {
        "tracked_files": len(files), "source_files_scanned": len(sources),
        "git_submodules_present": (ROOT / ".gitmodules").exists(),
        "vendor_directories": vendor_dirs, "external_link_flags": link_flags,
        "library_file_candidates": lib_files, "include_references": include_hits,
        "source_provenance_markers": provenance, "build_test_tools": detected,
        "github_actions_references": actions, "x86_grub_multiboot2_in_iso": bool(grub),
        "host_ca_bundle_copied_to_disk": bool(ca_bundle),
        "freestanding_no_libc_link_evidence": bool(no_libc_evidence),
    }
    out = ROOT / "ci-out" / "audit"
    out.mkdir(parents=True, exist_ok=True)
    (out / "dependency_inventory.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    lines = [
        "# KIRA OS — аудит зависимостей", "",
        "Статический анализ исходников и build scripts. Он не заменяет бинарный анализ ISO/KiraFS и security review.", "",
        f"- Tracked files: {len(files)}; production C/ASM/header files scanned: {len(sources)}.",
        f"- External linker -l flags: {len(link_flags)}; library-file candidates: {len(lib_files)}.",
        f"- Git submodules: {'yes' if report['git_submodules_present'] else 'no'}; vendor directories: {len(vendor_dirs)}.",
        f"- Freestanding/no-libc evidence in build flags: {'yes' if no_libc_evidence else 'not confirmed'}.", "",
        "## Components and tools", "",
        "| Component | Classification |", "|---|---|",
        f"| GRUB 2 / Multiboot2 | {'Bundled in x86 ISO; external boot-chain dependency' if grub else 'not detected'} |",
        f"| Shared/static library files | {'Candidates found: ' + ', '.join(lib_files) if lib_files else 'No tracked library files detected; binary image scan still required'} |",
        f"| CA roots | {'Host CA bundle copied to /etc/ssl/roots.der (trust data)' if ca_bundle else 'not confirmed'} |",
    ]
    lines += [f"| {x['tool']} | {x['role']} |" for x in detected]
    lines += ["", "## Source provenance markers", ""]
    lines += [f"- {x['file']}:{x['line']}: {x['text']}" for x in provenance[:80]] or ["- No markers found."]
    lines += ["", "## Remaining independence/security blockers", "",
        "- Replace GRUB/Multiboot2 with KIRA's own BIOS and UEFI loader.",
        "- kcc supports a C subset; full build still uses external Clang/LLD/NASM and wasm-ld.",
        "- RTL8139 rl(4) is a BSD-attributed port based on FreeBSD driver behavior; a specification-based rewrite is required for a strict all-original-code policy.",
        "- SHA-256 alone does not authenticate package publishers; add signed manifests, trusted keys and anti-rollback checks.",
        "- DMA-capable drivers need IOMMU domains or DMA must be disabled for untrusted drivers.",
        "- QEMU results do not validate physical x86/ARM hardware.", "",
        "## Scanner limits", "",
        "- Include resolution and linker flags are heuristic; generated headers and compiler-provided headers are classified separately.",
        "- The script does not unpack final ISO or KiraFS images. Run a binary-level audit on the produced artifacts before release.",
        "- Provenance markers require human review; they are not a license verdict.", "",
    ]
    (out / "DEPENDENCIES.md").write_text("\n".join(lines))
    print(f"dependency audit: {len(files)} files; {len(link_flags)} external link flags; {len(lib_files)} library candidates")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
