"""Streaming summaries for QEMU guest_errors logs.

The log begins at QEMU process start and can include firmware and guest activity.
A ready offset is a best-effort file-size snapshot taken when KIRA's console prompt
is observed; it is not a hardware-timestamped ownership boundary.
"""
import os


QEMU_DIAGNOSTIC_PATTERNS = {
    "invalid_reads": b"Invalid read at addr",
    "invalid_writes": b"Invalid write at addr",
}


def count_file_pattern(path, pattern, start=0, end=None, chunk_size=1024 * 1024):
    """Count a byte pattern in a bounded file range without loading the log into RAM."""
    if not isinstance(pattern, bytes) or not pattern:
        raise ValueError("pattern must be non-empty bytes")
    if chunk_size < 1:
        raise ValueError("chunk_size must be positive")
    size = os.path.getsize(path)
    if start < 0 or start > size:
        raise ValueError("start is outside the file")
    if end is None:
        end = size
    if end < start or end > size:
        raise ValueError("end is outside the file range")
    count = 0
    overlap = b""
    remaining = end - start
    with open(path, "rb") as f:
        f.seek(start)
        while remaining:
            block = f.read(min(chunk_size, remaining))
            if not block:
                break
            data = overlap + block
            count += data.count(pattern)
            overlap = data[-(len(pattern) - 1):] if len(pattern) > 1 else b""
            remaining -= len(block)
    return count


def count_file_patterns(path, patterns, boundary=None, chunk_size=1024 * 1024):
    """Count several patterns in one streaming pass, separating bytes before/after a phase boundary."""
    if chunk_size < 1 or not patterns or any(not isinstance(p, bytes) or not p for p in patterns.values()):
        raise ValueError("patterns and positive chunk_size are required")
    size = os.path.getsize(path)
    if boundary is not None and (boundary < 0 or boundary > size):
        raise ValueError("boundary is outside the file")
    phases = ("all",) if boundary is None else ("before", "after")
    counts = {phase: {name: 0 for name in patterns} for phase in phases}
    overlaps = {phase: b"" for phase in phases}
    keep = max(len(p) for p in patterns.values()) - 1

    def consume(data, phase):
        if not data:
            return
        combined = overlaps[phase] + data
        for name, pattern in patterns.items():
            counts[phase][name] += combined.count(pattern)
        overlaps[phase] = combined[-keep:] if keep else b""

    offset = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk_size)
            if not block:
                break
            if boundary is None:
                consume(block, "all")
            else:
                before = max(0, min(len(block), boundary - offset))
                consume(block[:before], "before")
                consume(block[before:], "after")
            offset += len(block)
    return counts


def summarize_qemu_log(path, ready_offset):
    """Split guest_errors at the best-effort byte offset captured at KIRA console readiness."""
    if not os.path.isfile(path):
        return {"path": os.path.basename(path), "exists": False, "log_bytes": 0,
                "ready_boundary_bytes": ready_offset, "boundary_status": "log-missing"}
    size = os.path.getsize(path)
    valid_boundary = isinstance(ready_offset, int) and 0 <= ready_offset <= size
    report = {
        "path": os.path.basename(path),
        "exists": True,
        "log_bytes": size,
        "ready_boundary_bytes": ready_offset,
        "boundary_status": "available" if valid_boundary else
            ("not-recorded" if ready_offset is None else "invalid-after-log-truncation"),
    }
    counts = count_file_patterns(path, QEMU_DIAGNOSTIC_PATTERNS, ready_offset if valid_boundary else None)
    for name in QEMU_DIAGNOSTIC_PATTERNS:
        if valid_boundary:
            before = counts["before"][name]
            after = counts["after"][name]
            report[name + "_total"] = before + after
            report[name + "_before_guest_ready"] = before
            report[name + "_after_guest_ready"] = after
        else:
            report[name + "_total"] = counts["all"][name]
            report[name + "_before_guest_ready"] = None
            report[name + "_after_guest_ready"] = None
    return report
