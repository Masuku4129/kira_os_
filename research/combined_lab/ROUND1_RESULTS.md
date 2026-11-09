# KIRA Combined Technology Lab — first measured results

Date: 2026-11-09  
Research branch: `research/combined-tech-lab-2026-11`  
Base OS point: `v46/unified-release-integration`, code point `13a9773`; branch documentation head `d7692bb`  
Latest measured lab commit: `dfd3ce10716e8b55f269c589aec91cee805c0134`  
CI: [run 42551324373](https://github.com/Masuku4129/kira_os_/actions/runs/42551324373)  
Raw results: [GitHub Actions artifact](https://github.com/Masuku4129/kira_os_/actions/runs/42551324373)

## What was actually assembled

This is a host-side prototype lab, not yet code linked into the KIRA kernel. Four technology combinations are implemented in one repeatable test program. The correctness gates run before measurements are accepted.

| Template | Combined technologies | Data representation / files | Test result |
|---|---|---|---|
| Typed IPC queue | fixed binary records + bounded ring buffer + capability check | little-endian fixed record `<QQiII>`, 28 bytes | Round-trip PASS; missing capability denied |
| Immutable storage | content-addressed objects + SHA-256 + Merkle manifest + copy-on-write snapshots | JSON chunk objects; SHA-256 object names; JSON manifests | Integrity tamper detected; prior snapshot rollback PASS |
| Mind telemetry | columnar layout + compressed cold segment | row JSON vs five typed little-endian columns; zlib | Exact round-trip PASS |
| Compiler artifact cache | canonical IR serialization + SHA-256 cache key + compiler/target/options fingerprint | canonical JSON IR; JSON result report | Warm hit PASS; target change invalidates cache |

## Measured results

Environment: GitHub Actions, Ubuntu 24.04.  
Input: 20,000 deterministic mixed event records.  
Timing: median of 7 repetitions in one CI run.  
These are host Python prototype measurements; they are not kernel or physical-hardware measurements.

| Combination | Baseline | Combined | Size / storage | Correctness |
|---|---:|---:|---:|---|
| Fixed binary copy path vs ring+capability | 7.887 ms | 20.037 ms | 560,000-byte payload | PASS |
| Full-copy snapshots vs CAS+Merkle+CoW | 3.575 ms | 2.928 ms | 26,062,992 logical snapshot bytes vs 650,459 unique object bytes | PASS |
| Row JSON+zlib vs columnar binary+zlib | 41.081 ms | 39.319 ms | 212,405 vs 117,400 bytes (1.81× smaller) | PASS |
| Cold canonical-IR cache vs warm cache | 0.011351 ms | 0.005641 ms | warm lookup ~2.01× faster in this tiny synthetic key test | PASS |

## Interpretation — no marketing claims

1. **IPC ring + capability:** the current Python ring implementation is about 2.54× slower than the simple copy path. It does not win on speed. Keep the capability concept for security, but do not select this ring design for performance until a true cross-process shared-memory implementation is benchmarked in C and under contention.
2. **CAS + Merkle + CoW:** the test stores each immutable chunk once and creates small version manifests. For this snapshot workload, the logical payload retained across versions is about 40.1× smaller; the combined operation was also about 18% faster than the baseline that physically recopies prior snapshot payloads. This is promising for package/update history and rollback, but not yet a KiraFS crash-consistency proof.
3. **Columnar + compression:** this combination is the clearest current data win: 1.81× smaller and about 4.3% faster to encode than row JSON+zlib for this workload. It is a candidate for cold Mind telemetry, not for hot IPC. Only zlib was tested; Zstd is still outstanding.
4. **Canonical IR cache:** the warm lookup was about 2× faster than the cold key/build path, but absolute times are only microseconds because the prototype hashes a tiny synthetic IR and does not invoke KCC or MLIR. This validates key invalidation logic, not real compiler acceleration.

## Exact formats and filenames

- IPC record: `<QQiII>` little-endian, 28 bytes per event.
- IPC test permission: explicit token `KIRA_IPC_SEND:demo-service`; absent token must be rejected.
- Storage object: JSON-encoded event chunk; object filename/key is SHA-256 hex digest.
- Storage manifest: JSON with version, ordered object hashes, and Merkle root.
- Telemetry: JSON row baseline; binary columnar payload with 64-bit, 64-bit, signed 32-bit, 32-bit, 32-bit columns; compressed with zlib level 6.
- Compiler cache key: SHA-256 over canonical JSON IR + compiler identity + target + build flags.
- Test outputs: `results.json` and `results.csv`.

## What is not yet tested

- True shared-memory IPC between separate processes, lock-free atomics, multi-producer/multi-consumer contention, and p95/p99 latency.
- FlatBuffers, Cap'n Proto, Arrow, Parquet, CBOR, Zstd, LZ4, and actual WASM decoder sandboxing.
- Real KiraFS block-device writes, flush ordering, power-loss injection, persistent Merkle manifests, Ed25519 package signatures, and secure rollback state.
- Actual Mind event capture and selective column reads.
- Real KCC/MLIR compilation, ELF equivalence, cache correctness under dependency changes, and x86-64/ARM64 code generation.
- Kernel integration, QEMU guest behavior, and physical hardware.

## Decision after round 1

- **Advance to round 2:** CAS + Merkle + CoW; columnar telemetry + compression.
- **Redesign before advancing:** ring + capability IPC. Security check passed, speed did not.
- **Keep as a promising mechanism, but demand real compiler integration:** canonical IR cache.
- Do not merge into `main`; all work remains isolated on the research branch.

## Next round

1. Implement process-shared ring IPC in C with acquire/release atomics, explicit capability validation, and competing producer/consumer processes.
2. Add actual Zstd/LZ4 and Arrow/FlatBuffers/Cap'n Proto/CBOR/Parquet codecs where supported; benchmark encode, decode, random access, malformed input, and peak memory.
3. Add storage crash injection at each write/flush boundary and verify old-or-new snapshot recovery.
4. Connect cache keys to real KCC IR, object files, target triple, compiler version, flags, and dependency digests.
5. Test combined workloads under simultaneous telemetry writes, storage snapshots, IPC traffic, and compilation rather than isolated microbenchmarks.

**Status:** first integrated prototype round completed. It produced two promising combinations, one IPC design that loses on speed, and one compiler-cache mechanism that still needs real compiler measurements. No architecture has been approved yet.
