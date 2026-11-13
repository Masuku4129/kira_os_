## User request and acceptance criteria

- [ ] Exact user request / requested outcome is captured.
- [ ] Explicit constraints, non-goals and safety boundaries are listed.
- [ ] Each requirement is mapped to the master plan (A–N) and one or more of the 12 parallel workstreams.

## Architecture and dependency impact

- [ ] Relevant architecture level(s) and subsystem contracts are identified.
- [ ] Callers, consumers, build manifests, x86-64 and ARM64 paths are checked.
- [ ] Component inventory, dependency map, contract registry and architecture guards are updated where needed.
- [ ] No duplicate implementation or unconnected production module is introduced.
- [ ] No stub, empty function, fake production path or unsupported capability claim is introduced.

## Requirement-to-evidence matrix

| Requirement ID | User requirement | Master plan A–N / workstream 1–12 | Code / contract / test | Exact SHA | Command / environment / result | Status |
|---|---|---|---|---|---|---|
| R-01 | | | | | | UNVERIFIED |

Allowed status values: PASS, PARTIAL, FAIL, UNVERIFIED, BLOCKED. PASS requires fresh evidence on the exact PR head.

## Verification

- [ ] Host tests
- [ ] x86-64 build
- [ ] ARM64 build
- [ ] Architecture / contract guards
- [ ] Relevant QEMU / OVMF scenarios
- [ ] Limbo smoke test (when relevant)
- [ ] Physical hardware test (only if actual hardware was used)
- [ ] Security / negative / failure-path tests relevant to the change
- [ ] Exact-head CI links and per-job results are recorded

Commands and environments:
```text
# command | environment | exact commit SHA | exit/result
```

## Final compliance review

- [ ] Every user requirement is satisfied or explicitly marked PARTIAL / BLOCKED / UNVERIFIED.
- [ ] No requirement from the master plan was silently removed or narrowed.
- [ ] Tests prove behavior rather than merely inspect source text.
- [ ] Diff contains only scoped changes; no unrelated cleanup or destructive action.
- [ ] WORKLOG and project status documents are updated with exact evidence.
- [ ] No merge to main is implied by this PR.
