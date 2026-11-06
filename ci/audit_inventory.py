#!/usr/bin/env python3
"""Inventory and static preflight for KIRA OS; standard library only."""
from __future__ import annotations

import argparse
import ast
import json
import re
import subprocess
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".c", ".h", ".S", ".s", ".py", ".sh"}
MARKER_WORDS = (
    "TO" + "DO", "FIX" + "ME", "XX" + "X", "HA" + "CK",
    "st" + "ub", "not" + " implemented", "un" + "implemented",
    "Not" + "Implemented", "заглуш" + "ка", "заготов" + "ка",
    "пока не" + " реализ", "имита" + "ция", "fa" + "ke",
    "du" + "mmy", "place" + "holder",
)
MARKER_RE = re.compile("|".join(re.escape(x) for x in MARKER_WORDS), re.IGNORECASE)
TOOLS = {
    "clang": "host/kernel and userland compiler",
    "ld.lld": "linker",
    "nasm": "x86 assembler",
    "grub-mkrescue": "x86 ISO bootloader/image creation",
    "xorriso": "ISO image tooling",
    "llvm-objcopy": "ARM64 image tooling",
    "wasm-ld": "WebAssembly linker",
    "qemu-system-x86_64": "x86_64 integration testing",
    "qemu-system-aarch64": "ARM64 integration testing",
    "gcc": "host tests and helper tools",
    "python3": "CI/test orchestration",
    "openssl": "TLS interoperability tests",
    "dosfstools": "FAT test images and validation",
    "mtools": "FAT test images and validation",
    "python3-cryptography": "certificate/crypto test oracle",
    "python3-pil": "image fixture and screenshot tests",
    "KVM": "accelerated x86_64 QEMU tests",
}

def git_output(*args: str) -> str:
    p = subprocess.run(["git", *args], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return p.stdout if p.returncode == 0 else ""

def tracked_files() -> list[str]:
    raw = git_output("ls-files", "-z")
    if raw:
        return sorted(x for x in raw.split("\0") if x)
    return sorted(str(p.relative_to(ROOT)) for p in ROOT.rglob("*") if p.is_file() and ".git" not in p.parts)

def scenario_counts(files: list[str]) -> tuple[list[str], list[str]]:
    ci = (ROOT / ".github/workflows/ci.yml").read_text(errors="replace") if (ROOT / ".github/workflows/ci.yml").exists() else ""
    match = re.search(r"scenario:\s*\[([^\]]+)\]", ci)
    x86 = [x.strip() for x in match.group(1).split(",") if x.strip()] if match else []
    arm_path = ROOT / "ci/arm64_vm.py"
    arm_text = arm_path.read_text(errors="replace") if arm_path.exists() else ""
    arm = sorted(set(re.findall(r"['\"](arm64-[a-z0-9-]+)['\"]", arm_text)))
    return x86, arm

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="ci-out/audit")
    args = ap.parse_args()
    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)

    files = tracked_files()
    suffix_counts = Counter(Path(p).suffix or "[no extension]" for p in files)
    c_files = sum(Path(p).suffix == ".c" for p in files)
    h_files = sum(Path(p).suffix == ".h" for p in files)
    test_files = [p for p in files if p.startswith("tests/")]
    workflows = [p for p in files if p.startswith(".github/workflows/")]
    x86_scenarios, arm_scenarios = scenario_counts(files)

    shell_errors: list[str] = []
    python_errors: list[str] = []
    marker_hits: list[dict[str, object]] = []
    for rel in files:
        path = ROOT / rel
        suffix = path.suffix
        if suffix == ".sh":
            p = subprocess.run(["bash", "-n", str(path)], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if p.returncode:
                shell_errors.append(f"{rel}: {p.stdout.strip()[:500]}")
        elif suffix == ".py":
            try:
                ast.parse(path.read_text(errors="replace"), filename=rel)
            except SyntaxError as exc:
                python_errors.append(f"{rel}:{exc.lineno}: {exc.msg}")
        if suffix in SOURCE_SUFFIXES and rel not in {"ci/lint.sh", "ci/audit_inventory.py"} and not rel.startswith("tests/"):
            try:
                lines = path.read_text(errors="replace").splitlines()
            except OSError:
                continue
            for line_no, line in enumerate(lines, 1):
                if MARKER_RE.search(line):
                    marker_hits.append({"file": rel, "line": line_no, "text": line.strip()[:240]})

    staged = git_output("ls-files", "--stage")
    submodules = [line for line in staged.splitlines() if line.startswith("160000 ")]
    gitmodules = (ROOT / ".gitmodules").exists()

    combined = ""
    for rel in [".github/workflows/ci.yml", "build_clang.sh", "build_arm64.sh", "run_qemu.sh", "userland/wasm/build.sh", "tests/run_host_tests.sh"]:
        p = ROOT / rel
        if p.exists():
            combined += "\n" + p.read_text(errors="replace")
    tool_rows = [{"tool": name, "role": role} for name, role in TOOLS.items()
                 if re.search(r"(?<![A-Za-z0-9_-])" + re.escape(name) + r"(?![A-Za-z0-9_-])", combined, re.IGNORECASE)]
    action_text = "\n".join((ROOT / p).read_text(errors="replace") for p in workflows if (ROOT / p).exists())
    action_refs = sorted(set(re.findall(r"uses:\s*([^\s#]+)", action_text)))

    progress = (ROOT / "docs/PROGRESS.md").read_text(errors="replace") if (ROOT / "docs/PROGRESS.md").exists() else ""
    arch = (ROOT / "ARCHITECTURE.md").read_text(errors="replace") if (ROOT / "ARCHITECTURE.md").exists() else ""
    handoff = (ROOT / "docs/HANDOFF.md").read_text(errors="replace") if (ROOT / "docs/HANDOFF.md").exists() else ""
    gaps = [
        "x86_64 ISO creation currently depends on GRUB/Multiboot2; no independent KIRA BIOS/UEFI loader was found in the tracked tree.",
        "The current build still uses external Clang/LLD/NASM (and wasm-ld); kcc is a limited in-OS C compiler, not yet the kernel's complete self-hosting toolchain.",
        "drvget accepts selected BSD-style source recipes and rejects Linux .ko and Windows .sys binaries; general Linux driver translation remains future work.",
        "Driver package records use explicit user confirmation and SHA-256; project progress notes say package signatures are absent. A hash is integrity evidence, not publisher authentication.",
        "The driver UI warns that there is no IOMMU; DMA-capable device isolation is therefore incomplete.",
        "ARM64 and x86_64 have QEMU coverage, but physical boards and real PCI/USB/network hardware have not been validated by these CI runs.",
        "The ARM64 non-coherent-DMA scenario is a QEMU model; the physical non-coherent hardware path remains unverified.",
    ]
    if "_PRT" in progress or "_PRT" in handoff:
        gaps.append("ACPI AML _PRT routing is listed as unfinished; some legacy INTx routing remains incomplete.")
    work_path = ROOT / "docs/WORKFLOW.md"
    work_text = work_path.read_text(errors="replace") if work_path.exists() else ""
    if "Claude Sonnet" in work_text:
        gaps.append("docs/WORKFLOW.md still contains Claude-specific branch/commit instructions and needs to be made tool-neutral.")
    if "13 сценариев" in handoff or "31.10.2026" in handoff:
        gaps.append("docs/HANDOFF.md is stale relative to the current CI matrix and docs/PROGRESS.md.")
    if "Ничего из этого не запускалось в QEMU" in arch:
        gaps.append("ARCHITECTURE.md contains historical status text that contradicts the current QEMU CI coverage; docs need one authoritative current status.")

    inventory = {
        "files": len(files), "c_files": c_files, "h_files": h_files,
        "test_files": len(test_files), "workflow_files": workflows,
        "suffix_counts": dict(sorted(suffix_counts.items())),
        "x86_qemu_scenarios_in_workflow": x86_scenarios,
        "arm64_qemu_scenarios_in_runner": arm_scenarios,
        "external_tools_detected_in_build_and_ci": tool_rows,
        "github_actions_refs": action_refs,
        "gitmodules_present": gitmodules, "git_submodule_entries": len(submodules),
        "shell_syntax_errors": shell_errors, "python_syntax_errors": python_errors,
        "source_marker_hits": marker_hits, "known_architecture_gaps": gaps,
    }
    (out / "inventory.json").write_text(json.dumps(inventory, ensure_ascii=False, indent=2) + "\n")

    lines = [
        "# KIRA OS — автоматическая инвентаризация и базовый аудит",
        "",
        "> Это автоматический инвентаризационный отчёт, а не заявление о полном доказательстве корректности ядра. Смысловые выводы требуют проверки исходников и поведения на стенде.",
        "",
        "## Состав репозитория",
        "",
        f"- Отслеживаемых файлов: **{len(files)}**",
        f"- Файлов C: **{c_files}**; заголовков: **{h_files}**",
        f"- Файлов в tests/: **{len(test_files)}**",
        f"- Workflow GitHub Actions: **{len(workflows)}**",
        f"- Сценариев x86_64 в CI: **{len(x86_scenarios)}**",
        f"- Сценариев ARM64 в runner: **{len(arm_scenarios)}**",
        f"- Git submodules: **{len(submodules)}**; .gitmodules: **{'есть' if gitmodules else 'нет'}**",
        "",
        "## Проверки синтаксиса",
        "",
        f"- Bash: {'ошибок нет' if not shell_errors else f'{len(shell_errors)} ошибок'}",
        f"- Python AST: {'ошибок нет' if not python_errors else f'{len(python_errors)} ошибок'}",
        "- C/ASM компиляция и исполняемые проверки остаются в существующих host-tests/build/QEMU jobs.",
        "",
        "## Внешние инструменты и зависимости",
        "",
        "| Инструмент | Роль в сборке/проверках |",
        "|---|---|",
    ]
    lines.extend(f"| {row['tool']} | {row['role']} |" for row in tool_rows)
    lines += [
        "",
        "Классификация: Clang/LLD/NASM/wasm-ld и GRUB относятся к сборке; QEMU/KVM, GCC, OpenSSL, Python-модули, dosfstools и mtools — к тестированию и стенду. GRUB попадает в x86_64 ISO и потому остаётся внешней runtime-зависимостью загрузки. Наличие этих инструментов в CI не означает, что они входят в запущенное ядро.",
        "",
        "## Известные архитектурные ограничения",
        "",
    ]
    lines.extend(f"- {gap}" for gap in gaps)
    lines += [
        "",
        "## Поиск маркеров незавершённой реализации",
        "",
        f"Найдено совпадений-маркеров в производственных исходниках: **{len(marker_hits)}**. Это эвристика: совпадение требует ручной оценки и само по себе не доказывает наличие незавершённой реализации.",
    ]
    if marker_hits:
        lines += ["", "| Файл | Строка | Фрагмент |", "|---|---:|---|"]
        lines.extend(f"| {x['file']} | {x['line']} | {str(x['text']).replace('|', '&#124;')} |" for x in marker_hits[:100])
        if len(marker_hits) > 100:
            lines.append(f"| … | … | Ещё {len(marker_hits) - 100} совпадений см. inventory.json |")
    lines += [
        "",
        "## Ограничения этого отчёта",
        "",
        "- Список инструментов извлекается из основных build/CI scripts и не заменяет анализ каждого include/import и всех linker inputs.",
        "- Поиск маркеров не способен доказать отсутствие пустых функций, неверной логики, гонок или скрытых незавершённых реализаций.",
        "- Успешные QEMU-прогоны не подтверждают совместимость с физическим оборудованием.",
        "- Точные результаты текущего запуска находятся в TEST_REPORT.md, который формируется отдельно после завершения всех jobs.",
        "",
    ]
    (out / "AUDIT.md").write_text("\n".join(lines))
    print(f"audit: {len(files)} tracked files; {len(x86_scenarios)} x86 and {len(arm_scenarios)} ARM64 scenario definitions")
    print(f"audit: bash syntax errors={len(shell_errors)}, Python syntax errors={len(python_errors)}, source marker hits={len(marker_hits)}")
    return 1 if shell_errors or python_errors else 0

if __name__ == "__main__":
    raise SystemExit(main())
