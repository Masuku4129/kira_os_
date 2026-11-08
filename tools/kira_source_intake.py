#!/usr/bin/env python3
"""KIRA source intake prototype.

Safely preserves an upstream driver source archive as a .ksrc package.
It does NOT compile or execute imported code. API compatibility and runtime
eligibility remain separate, explicit decisions.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import os
import re
import stat
import tarfile
import zipfile
from datetime import datetime, timezone
from pathlib import PurePosixPath

MAX_FILES = 20000
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
SOURCE_SUFFIXES = {".c", ".h", ".S", ".s", ".rs", ".zig", ".cpp", ".hpp", ".inc"}
SPDX_RE = re.compile(rb"SPDX-License-Identifier:\s*([^\r\n*]+)")
LICENSE_NAMES = {"LICENSE", "LICENSE.txt", "COPYING", "COPYING.LESSER", "NOTICE"}
PERMISSIVE_SPDX = {"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "Zlib", "0BSD"}
COPYLEFT_PREFIXES = ("GPL-", "LGPL-", "AGPL-", "MPL-", "EPL-", "CDDL-", "OSL-")


class IntakeError(ValueError):
    pass


def safe_name(name: str) -> str:
    name = name.replace("\\", "/")
    p = PurePosixPath(name)
    if not name or name.startswith("/") or any(x in ("", ".", "..") for x in p.parts):
        raise IntakeError(f"unsafe archive path: {name!r}")
    if ":" in p.parts[0] or "\x00" in name:
        raise IntakeError(f"unsafe archive path: {name!r}")
    return str(p)


def read_archive(path: str) -> dict[str, bytes]:
    items: dict[str, bytes] = {}
    total = 0
    def add(name: str, data: bytes):
        nonlocal total
        name = safe_name(name)
        if name in items:
            raise IntakeError(f"duplicate path: {name}")
        if len(data) > MAX_FILE_BYTES:
            raise IntakeError(f"file too large: {name}")
        total += len(data)
        if total > MAX_TOTAL_BYTES:
            raise IntakeError("archive exceeds total uncompressed size limit")
        items[name] = data
        if len(items) > MAX_FILES:
            raise IntakeError("archive contains too many files")

    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                mode = info.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise IntakeError(f"symlink not accepted: {info.filename}")
                if info.flag_bits & 1:
                    raise IntakeError(f"encrypted member not accepted: {info.filename}")
                if info.file_size > MAX_FILE_BYTES:
                    raise IntakeError(f"file too large: {info.filename}")
                add(info.filename, z.read(info))
    else:
        try:
            with tarfile.open(path, "r:*") as t:
                for m in t.getmembers():
                    if m.isdir():
                        continue
                    if not m.isfile():
                        raise IntakeError(f"non-regular archive member not accepted: {m.name}")
                    if m.size > MAX_FILE_BYTES:
                        raise IntakeError(f"file too large: {m.name}")
                    f = t.extractfile(m)
                    if f is None:
                        raise IntakeError(f"cannot read archive member: {m.name}")
                    add(m.name, f.read(MAX_FILE_BYTES + 1))
        except tarfile.TarError as e:
            raise IntakeError("input is neither a supported ZIP nor TAR archive") from e
    if not items:
        raise IntakeError("archive contains no files")
    return items


def inspect(items: dict[str, bytes], source_url: str) -> dict:
    spdx = set()
    license_files = []
    source_files = []
    for name, data in sorted(items.items()):
        base = PurePosixPath(name).name
        if base.upper() in {x.upper() for x in LICENSE_NAMES}:
            license_files.append(name)
        if PurePosixPath(name).suffix in SOURCE_SUFFIXES:
            source_files.append(name)
        for match in SPDX_RE.findall(data[:8192]):
            value = match.decode("utf-8", "replace").strip().rstrip("*/ ")
            if value:
                spdx.add(value)
    ids = sorted(spdx)
    if ids and all(x in PERMISSIVE_SPDX for x in ids):
        license_class = "PREFERRED_PERMISSIVE_REVIEW_REQUIRED"
    elif any(x.startswith(COPYLEFT_PREFIXES) for x in ids):
        license_class = "COPYLEFT_COMPATIBILITY_REVIEW_REQUIRED"
    elif ids:
        license_class = "NON_ALLOWLISTED_LICENSE_REVIEW_REQUIRED"
    elif license_files:
        license_class = "LICENSE_TEXT_FOUND_MANUAL_REVIEW_REQUIRED"
    else:
        license_class = "UNKNOWN_BLOCKED_FOR_DISTRIBUTION"
    return {
        "schema": "kira-source-package-v1",
        "source_url": source_url or "UNSPECIFIED",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "file_count": len(items),
        "source_file_count": len(source_files),
        "total_bytes": sum(map(len, items.values())),
        "source_sha256": hashlib.sha256(b"".join(
            name.encode() + b"\0" + hashlib.sha256(data).digest()
            for name, data in sorted(items.items())
        )).hexdigest(),
        "provenance": {
            "source_origin_status": "UNVERIFIED_SOURCE_ORIGIN",
            "policy": "Use official vendor/project repositories or explicitly authorized mirrors; verify publisher and pinned commit before production use"
        },
        "license": {
            "spdx_identifiers_found": ids,
            "license_text_files": license_files,
            "status": license_class,
            "preferred_spdx_allowlist": sorted(PERMISSIVE_SPDX),
            "policy": "PRESERVE_FOR_ANALYSIS; distribution, modification and derivative-work rights require separate review"
        },
        "vendor_policy": {
            "official_source_or_permission_verified": False,
            "closed_sdk_or_leaked_source": "REJECT",
            "unauthorized_access_or_bypass": "REJECT",
            "reverse_engineering": "ONLY_WHERE_LAWFUL_AND_AUTHORIZED"
        },
        "compatibility": {
            "target": "KIRA",
            "status": "NOT_ASSESSED",
            "unresolved_api_dependencies": [],
            "adapter_required": None
        },
        "build": {
            "status": "NOT_ATTEMPTED",
            "compiler": None,
            "target_architectures": [],
            "logs": []
        },
        "runtime": {
            "status": "BLOCKED_NOT_VALIDATED",
            "installable": False,
            "reason": "Source preservation is not proof of API compatibility, safe compilation, or runtime correctness"
        },
        "source_files": source_files
    }


def make_report(m: dict) -> str:
    lic = ", ".join(m["license"]["spdx_identifiers_found"]) or "не определена автоматически"
    return f"""# KIRA Source Intake Report

- Источник: {m['source_url']}
- Файлов: {m['file_count']}; исходных файлов: {m['source_file_count']}
- Размер исходников/пакета: {m['total_bytes']} байт
- SHA-256 набора: `{m['source_sha256']}`
- Лицензия: {lic} ({m['license']['status']})
- Происхождение источника: {m['provenance']['source_origin_status']}
- Совместимость с KIRA: НЕ ПРОВЕРЯЛАСЬ
- Сборка: НЕ ЗАПУСКАЛАСЬ
- Запуск в ОС: ЗАПРЕЩЁН ДО ПРОВЕРОК

Исходники сохранены без выполнения и без попытки скрыть несовместимость.
Даже если API невозможно адаптировать сейчас, пакет остаётся доступным для
дальнейшего анализа. Лицензионные условия и право распространения проверяются
отдельно; наличие исходника в пакете не означает разрешение на его включение
в дистрибутив KIRA.
"""


def build_package(input_path: str, output_path: str, source_url: str = "") -> dict:
    items = read_archive(input_path)
    manifest = inspect(items, source_url)
    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.writestr("manifest.json", json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        z.writestr("REPORT.md", make_report(manifest))
        for name, data in sorted(items.items()):
            z.writestr("source/" + name, data)
    return manifest


def main() -> int:
    ap = argparse.ArgumentParser(description="Safely preserve an upstream source archive as KIRA .ksrc")
    ap.add_argument("input", help="ZIP or TAR/TAR.GZ source archive")
    ap.add_argument("-o", "--output", required=True, help="output .ksrc package")
    ap.add_argument("--source-url", default="", help="upstream repository or release URL")
    args = ap.parse_args()
    try:
        m = build_package(args.input, args.output, args.source_url)
    except (OSError, IntakeError, zipfile.BadZipFile) as e:
        ap.error(str(e))
    print(json.dumps({
        "package": args.output,
        "files": m["file_count"],
        "license_status": m["license"]["status"],
        "compatibility": m["compatibility"]["status"],
        "runtime": m["runtime"]["status"],
        "source_sha256": m["source_sha256"]
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
