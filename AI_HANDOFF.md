# KIRA OS — AI / developer handoff

**Repository:** `Masuku4129/kira_os_`  
**Primary working branch:** `architecture/stage0-repository-audit-2026-11-14`  
**Current stage:** Stage 0 — IN PROGRESS.  
**Canonical stage board:** [`docs/STAGE_TRACKER.md`](STAGE_TRACKER.md)  
**Architecture and all stages:** [`docs/architecture/KIRA_TARGET_ARCHITECTURE_AND_STAGE_PLAN_2026-11-15.md`](architecture/KIRA_TARGET_ARCHITECTURE_AND_STAGE_PLAN_2026-11-15.md)

This file is the fast onboarding entry point for any new AI agent or human contributor. It is a navigation and operating contract, not a replacement for the full documents linked below.

## Start here — mandatory reading order

1. `AGENTS.md` — repository-wide operating rules.
2. `docs/MASTER_DEVELOPMENT_PLAN.md` — full project scope and requirements A–N.
3. `docs/WORKLOG.md` — chronological work history; find the latest entries, then verify their SHAs.
4. `docs/STAGE_TRACKER.md` — current stage, gates, package status and next queue.
5. `README.md`, `ARCHITECTURE.md`, relevant subsystem docs and ADRs.
6. `docs/architecture/STAGE0_REPOSITORY_AUDIT_2026-11-14.md`, component/file/build/test matrices, findings, PR graph and requirements crosswalk.

Do not begin implementation until this reading and a fresh GitHub state check are complete.

## Current working contract

- Stage 0 only. Audit, evidence gathering, architecture, test strategy, PR classification and documentation are in scope. Do not begin broad production migrations for later stages.
- Do not merge PRs, retarget branches, rewrite history, force-push, delete branches or alter `main` without explicit owner approval.
- Preserve every existing feature, source file, asset, build target and release artifact. Never replace a complete OS/ISO with a reduced build or call a partial image complete.
- No stubs, empty functions, fake production logic or mocks substituted for real implementations.
- Fix root causes, not symptoms. Re-run the exact affected tests after each change.
- A PASS must cite exact commit SHA, workflow run/job or local command, environment and actual result. CI on another SHA is not evidence for the current head.
- Separate host tests, x86-64 build, AArch64 build, QEMU, Limbo and physical hardware. Do not infer hardware support from emulator success.
- Treat branch-only code as a candidate, not as integrated functionality in `main`.
- Work in substantial batches. Avoid status-only replies and repeated plan narration. Update GitHub records as durable work is completed.
- If several agents work concurrently, each must claim a bounded file/subsystem area in the shared work log or issue before editing. Never have agents edit the same file concurrently without explicit coordination.

## Fresh-state procedure (must be repeated at every handoff)

1. Fetch current `main` SHA and working-branch SHA.
2. Fetch open PRs, head/base SHAs, changed files, review state and exact-head checks.
3. Identify commits made since the last recorded audit snapshot.
4. Read the latest `WORKLOG` entries and compare their SHAs to live GitHub.
5. Confirm that the Stage Tracker and findings match current evidence. Correct stale statements before starting new work.
6. Record the fresh baseline in the work log before code changes.

Known snapshot only (must be revalidated): on 2026-11-15, `main` was `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`; 92 open PRs were inventoried in `STAGE0_OPEN_PR_GRAPH_2026-11-15.md`. These are historical snapshot values, not guaranteed current state.

## Current confirmed work context (snapshot; verify before acting)

- Target architecture and Stage 0–13 map are drafted; they do not assert that proposed Rust/no_std, MLIR, typed IPC or new formats are already integrated.
- Requirements crosswalk A–N exists; it must be checked against the entire master plan for omissions.
- Open PR inventory exists, but all 92 diffs/ancestry/reviews/exact-head workflows have not been individually classified.
- PR #179/#181/#182 have a recorded shared build blocker F-062 on their observed heads; re-fetch before making decisions.
- F-016 reserve-map/PMM candidate PR #166 had 30/30 jobs successful on observed head `4bc69a224887ead8d8d1737e782ed76cf4b8e95e`, workflow `43610515593`. Candidate remains branch-only; this does not close main.
- Current-main findings include F-058 and F-061 string-boundary issues and F-063 FDT v16 header handling. Verify exact source before fixing; do not assume the candidate PR includes unrelated F-058/F-061 remediation.
- Do not apply speculative fixes to production code during the audit without establishing the full call graph, compatibility impact and regression tests.

## Required durable artifacts

- Every source finding: unique ID, severity, confidence, exact source location, trigger/impact, proof or reproduction, affected branches, remediation, regression tests, status.
- Every PR: head/base SHA, ancestry/dependencies, diff scope, unique changes, CI exact head, conflicts, classification and safe integration order.
- Every subsystem: source files, build targets, runtime dependencies, tests, architecture coverage, missing tests, current implementation status.
- Every architecture decision: ADR with context, alternatives, decision, consequences, migration and rollback.
- Every stage: exit criteria, dependencies, owner/work packages, tests and evidence, explicit gate result.
- Every session: append to `docs/WORKLOG.md` with exact commits, files, commands, results, unresolved blockers and next concrete tasks.

## Work allocation for parallel agents

An agent may be assigned one bounded package, for example:
- FDT/firmware parser audit and regression test inventory;
- x86-64 boot/interrupt path audit;
- AArch64 boot/GIC/PMM audit;
- scheduler/process/syscall source map;
- storage/filesystem integrity audit;
- networking/driver/IOMMU audit;
- WASM/KCC/compiler build and test map;
- Spatial Minimal UI/release asset completeness audit;
- PR stack ancestry/diff/CI classification.

Before editing, publish the exact files/subsystem owned, expected output and dependencies. Prefer read-only audit tasks until Stage 0 permits implementation. Merge findings into the canonical documents; do not create competing plans.

## How to resume in one instruction

“Read `AI_HANDOFF.md`, follow its mandatory reading order, refresh all GitHub state, inspect `docs/STAGE_TRACKER.md` and the latest work log, then continue the next uncompleted Stage 0 work package. Do not ask me to repeat repository context.”
