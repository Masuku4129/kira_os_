# KIRA Technology Lab: data formats

## Purpose

This experiment compares several representations of the same deterministic event records. It is deliberately host-side: it measures Python serialization/layout choices and does **not** claim to measure KiraFS, kernel memory copies, or hardware DMA.

## Tested representations

- Compact JSON: easy to inspect and exchange; textual and self-describing.
- Fixed-width binary records: predictable offsets and cheap sequential decoding.
- Columnar SoA: values of each field are contiguous; useful for scans, SIMD, and selective column access.
- Delta + varint TLV-like stream: exploits monotonic IDs/timestamps and small signed values; compact but branchy to decode.
- JSON + zlib and columnar + zlib: illustrates the storage-size/CPU trade-off.

Every case must round-trip to exactly the original records before metrics are accepted. The benchmark emits CSV and JSON with payload size, bytes per row, median encode/decode time, and rows per second.

## Reproduction

```bash
python3 benchmarks/data_formats.py --rows 50000 --repeat 3 --out ci-out/data-format-lab
```

GitHub Actions runs the same experiment and uploads `data-format-lab-results`.

## How to interpret

There is no universal winner. Fixed binary is a candidate for hot, typed kernel records; a columnar representation is a candidate for telemetry scans and analytics; delta/varint is a candidate for compact sequential logs when values have the tested distributions; JSON is useful at trust boundaries, diagnostics, and human-facing configuration. Compression should be selected by workload and CPU budget, not enabled indiscriminately.

## Candidate KIRA combinations to investigate next

1. **KiraFS + content-addressed immutable objects + Merkle manifests** for verified updates, deduplication, and rollback. Keep mutable metadata and transactional commit rules separate from immutable payload objects.
2. **Typed binary IPC + schema versioning + capability handles** for high-frequency local communication. Avoid parsing arbitrary JSON in privileged paths.
3. **Columnar telemetry + dictionary/delta encoding + background compression** for the Mind/AI history pipeline. Keep a bounded hot in-memory ring and compact older segments.
4. **Compiler cache keyed by normalized IR + toolchain/options digest** for reproducible incremental builds. Cache only after defining canonical IR serialization and invalidation rules.
5. **WASM or another bounded decoder sandbox for optional codecs** only after signature, resource limits, and deterministic decode rules are in place.

These are architecture hypotheses, not yet implemented KIRA features. The benchmark does not prove that any format is best for the kernel; a follow-up must measure the same encodings in C on the target toolchain and include random access, partial-field reads, corruption, and crash consistency.
