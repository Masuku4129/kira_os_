# RFC-MIND-001 — KIRA Mind telemetry, learning and bounded control

**Статус:** DRAFT — observed implementation and target control boundary.  
**База main:** `bbc7f7ab948e72511aed113b69043add10ec8350`.

## 1. What KIRA Mind is today

KIRA Mind is a kernel-resident adaptive resource-management and diagnostic controller, not a general-purpose LLM or an unrestricted agent.

Current implementation:
- `mind_sys.c` samples CPU load, runnable queues, memory pressure, process behavior, disk/IPC/syscall activity, audio timing and device telemetry.
- `mind.c` learns per-machine signal norms, tracks application profiles, chooses scheduler weights/quantum/limits, applies memory-pressure actions, controls optional visual effects and evaluates device events.
- `mindnet.c` is a small fixed-point logistic classifier: a weighted feature vector, sigmoid lookup table, online gradient updates and precision/recall/loss counters. It predicts a specific late-loop/latency risk, not arbitrary natural-language tasks.
- Model state is stored at `/var/mind.dat`, periodically saved and loaded with version migration and CRC validation.
- The main loop calls `mind_sys_tick()`; actions use callbacks directly into scheduler, process, memory, USB and display subsystems.

## 2. Current safety controls observed

- Modes distinguish observation from action (`MIND_OBSERVE` / `MIND_ACT`); most mutating actions require acting mode.
- Scheduler tuning uses measured trials and rollback when the score worsens.
- Memory response is staged: pressure thresholds at 88%, 93% and 97%; heap restriction and throttling precede destructive actions.
- Process kill is rate-limited to at most once per 10 seconds and only considered at the highest pressure stage when the largest process heap is at least 16 MiB and at least 20% of total RAM.
- Visual effects have user override and safety conditions.
- The model is data, not an authority source: CRC detects accidental corruption but does not authenticate the file or grant permissions.

These controls must be preserved and independently tested; the architecture must not imply that the current code already has a generic capability-checked control API.

## 3. Target architecture

Separate the control loop into explicit phases:

1. **Observe:** typed, read-only telemetry snapshots with source, timestamp, validity, units and confidence.
2. **Learn:** update per-machine norms and narrowly scoped predictive models; data quality and missing-sample semantics are explicit.
3. **Decide:** produce a typed proposal, not a direct side effect.
4. **Authorize:** the owner of the resource validates policy, capabilities, limits, current state, rate limits and user preferences.
5. **Apply:** resource owner performs the action transactionally where possible.
6. **Verify:** measure outcome against expected benefit and safety invariants.
7. **Rollback:** restore prior configuration if health or objective regresses.
8. **Audit:** record decision, input provenance, model version, policy decision, action result and rollback.

## 4. Control request contract

Every proposal includes:
- action type and schema version;
- requester identity (Mind instance/build/model version);
- target object ID + generation (process, scheduler class, device, display, CPU set);
- requested change and current value;
- reason code and supporting telemetry IDs;
- allowed range, resource budget and TTL;
- expected benefit and risk classification;
- rollback token or previous value where applicable.

The resource owner is authoritative. Mind cannot mint capabilities, bypass owner checks or call internal scheduler/page-table/device state directly. Destructive actions such as process termination require explicit system policy and user-configurable behavior.

## 5. Learning and model integrity

- Telemetry records distinguish missing, stale, invalid and zero values.
- Seeded/emulator data must be labeled and cannot be treated as physical-hardware truth.
- Training labels and outcomes must be causally aligned; no training on the same sample before evaluating its prediction.
- Model schema/version, feature order, fixed-point scale, calibration and validation metrics are recorded.
- CRC is corruption detection only. If model authenticity matters, use signed/authorized model updates or a trusted local ownership model; never treat model weights as policy.
- Training must be bounded in CPU/memory and rate-limited; corrupted or low-confidence models fall back to observation/default policy.
- Model updates must be atomic and recoverable across power loss.

## 6. Fault containment

- A Mind crash or corrupt model must not prevent boot, scheduling, memory allocation or user work.
- Failure to save model state must not block the main loop.
- Each action callback has bounded execution time and explicit failure result.
- Mind cannot hold locks while calling external services or blocking operations.
- Telemetry consumers must tolerate missed samples and version changes.
- Disable/observe-only mode must be available and tested.

## 7. Required tests

- observation-only mode never mutates scheduler/process/device state;
- action denied without owner authorization;
- boundary values and rate limits for every action;
- process kill thresholds, minimum heap, percentage-of-RAM and cooldown;
- scheduler tuning trial rollback under regressions;
- missing/stale/invalid telemetry never treated as zero;
- model corruption, old-version migration, seed labeling and power-loss during save;
- low-confidence/untrusted model falls back safely;
- Mind crash/restart while other kernel services continue;
- concurrent telemetry snapshots and action request races;
- exact outcome metrics and false-positive/false-negative evaluation for prediction.

## 8. Acceptance gate

KIRA Mind is production-ready only when every mutating action is enumerated and mapped to an authoritative resource owner, policy, limits, audit and rollback test. Existing thresholds are not enough to establish a unified security boundary.
