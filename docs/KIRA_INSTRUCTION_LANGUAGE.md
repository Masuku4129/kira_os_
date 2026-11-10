# KIRA Instruction Language (KIL) — design v0.2

## Purpose

KIL is a shared, reviewable instruction language for people and AI agents working with KIRA OS. It describes **goals, constraints, permissions, dependencies, parallel work, verification and acceptance** in one structured plan.

A person can read and edit the plan. An AI can draft or refine it. The KIL validator checks that the plan is well-formed and permitted. A separate executor runs only registered, typed actions and records evidence.

KIL is not a new native programming language, not a natural-language command interpreter, and not a shell wrapper. It does not replace C, Rust, assembly or KCC. The architecture-selection research found no measured basis for adding another general-purpose compiler frontend; KIL therefore starts as a declarative task/workflow language.

## Core rules

1. **Human-readable, machine-checkable.** Natural-language fields explain intent; typed fields determine execution.
2. **No arbitrary execution.** A task may invoke only named actions registered by the OS or a trusted tool provider. Unknown actions are rejected.
3. **No implicit authority.** `requires` requests capabilities; it never grants them. The policy engine and, where required, a human approval decide what is granted.
4. **Fail closed.** Missing inputs, unknown fields, invalid paths, dependency cycles, failed checks or unavailable permissions stop the plan.
5. **Auditable results.** Every run records the source hash, canonical plan, action versions, permissions, inputs, logs, outputs and artifact hashes.
6. **Parallel only when safe.** Independent tasks may run in separate build boxes. Conflicting writes must be declared or rejected.
7. **Deterministic plan semantics.** Given the same validated plan, action registry and inputs, the executor constructs the same dependency graph and acceptance rules. External action results are recorded, not fabricated.
8. **AI output is untrusted input.** AI-generated code or plans are proposals; tests, policy checks and required human review remain mandatory.

## Source form

KIL uses explicit blocks and typed values. UTF-8 text is allowed in `intent`, `context` and `notes`; identifiers and action names use stable ASCII spelling.

```kil
workspace KIRA version 1;

task "verify-x86-isolation" {
    intent "Проверить изоляцию приложений и отказ в неразрешённых системных вызовах";
    target x86_64;

    inputs {
        source_revision: git_revision;
        test_profile: "isolation";
    }

    requires {
        repository.read;
        runner.qemu;
        artifact.write("ci-out/**");
    }

    resources {
        cpu: 4;
        memory: 4 GiB;
        timeout: 15 min;
    }

    steps {
        ci.build {
            target: x86_64;
            profile: test;
        }

        ci.qemu_scenario {
            name: isolation;
            machine: q35;
            cpus: 4;
        }
    }

    verify all {
        result.build == success;
        result.scenario == success;
        evidence.serial_log.exists;
        evidence.no_kernel_faults;
    }

    accept all {
        evidence.address_spaces_isolated;
        evidence.unauthorized_syscalls_denied;
        evidence.ipc_roundtrips == 200;
        evidence.kernel_remains_responsive;
    }

    on_failure {
        preserve_logs;
        preserve_failed_artifacts;
        do_not_publish;
    }

    publish: forbidden;
}
```

This is a design example. Names such as `ci.build` and `ci.qemu_scenario` become executable only when their handlers are explicitly registered. A parser must never treat an unregistered action as a shell command.

## Language model

### Top-level declarations

- `workspace`: project namespace and language version.
- `task`: a unit of work with an immutable identifier, intent and acceptance rules.
- `parallel`: a group of tasks that may run concurrently when their dependency and resource constraints permit.
- `policy`: optional project-wide limits inherited by tasks.
- `include`: import another KIL document by a content-addressed, trusted reference.

### Task sections

| Section | Meaning |
|---|---|
| `intent` | Human-readable desired outcome; not executable |
| `context` | Background and assumptions supplied to the human or AI |
| `target` | Architecture, device class or environment |
| `inputs` | Typed inputs with provenance |
| `outputs` | Declared artifacts and their expected types |
| `depends` | Tasks that must complete first |
| `requires` | Requested capabilities and resource access |
| `resources` | CPU, memory, time, storage and concurrency limits |
| `steps` | Ordered registered actions |
| `verify` | Machine-evaluated checks over results and evidence |
| `accept` | Conditions that must all hold before completion |
| `human_review` | Mandatory review/approval gate |
| `on_failure` | Preserve, rollback or quarantine behavior |
| `publish` | Explicit publication policy; default is forbidden |

### Types

The first version needs only a small closed type system:

- scalar: `bool`, `int`, `string`, `duration`, `bytesize`;
- identifiers: `architecture`, `device_id`, `capability`, `action_id`;
- references: `path`, `digest`, `git_revision`, `artifact_ref`;
- containers: fixed lists and records with declared fields.

There is no arbitrary function call, pointer, dynamic import, reflection or user-defined code execution in v1.

### Execution model

1. Parse the document into an AST and report source locations for every error.
2. Validate schema, types, action registry, path policy, resource bounds and capability requests.
3. Build a dependency DAG; reject cycles and undeclared write conflicts.
4. Resolve a plan against the trusted action registry and policy. Ask for human approval where required.
5. Run independent tasks in isolated boxes. Each box receives only its declared inputs and capabilities.
6. Collect immutable evidence and artifact hashes.
7. Evaluate `verify` and `accept`; publish only if every required gate passes and the policy explicitly allows it.

The executor does not infer success from an AI statement, a log substring alone or an unverified artifact name. Evidence must be produced by the registered action and checked by the corresponding verifier.

## AI and human collaboration

- A human may write the whole plan or only the intent and acceptance criteria.
- An AI may propose missing fields, dependency graphs, test plans or patches.
- The AI must label assumptions and unknowns; it cannot silently weaken acceptance criteria.
- Changes to `requires`, `accept`, `publish`, destructive actions or trust policy require explicit review.
- A human-readable `notes` field may explain why a choice was made, but notes never override typed policy.
- The executor records which actor authored or changed each plan revision.

## Security model

- No shell execution by default. A future `process.exec` action, if ever added, must be sandboxed, allow-listed and separately granted.
- Network access is destination- and protocol-scoped.
- File access is path-scoped; traversal, symlink escape and undeclared writes are rejected.
- Secrets are references to protected secret handles, never literal values in KIL source or logs.
- Build boxes have isolated workspaces and cannot publish artifacts directly.
- Signing keys remain outside ordinary build boxes; signing is a separately authorized action.
- The KIL file cannot grant kernel capabilities or change an application's signed manifest.

## Initial integration with the parallel build factory

The first production use should be the existing nine-box CI workflow. Each box becomes a task with declared inputs, resource limits, action IDs, evidence and acceptance rules. This tests whether KIL improves reviewability and orchestration before the OS gains a general-purpose KIL runtime.

The first implementation should be a parser, validator and canonical plan serializer in userland. The executor should reuse the existing CI/build services; it must not introduce a second compiler IR or machine-code backend.

## Conformance tests before accepting an implementation

1. Lexer/parser: UTF-8 intent text, comments, escapes, malformed blocks and precise source locations.
2. Schema/types: unknown fields, invalid types, missing required values and duplicate task identifiers.
3. Dependency graph: topological order, cycles, parallel eligibility and conflicting writes.
4. Policy: undeclared action, capability escalation, untrusted action registry and missing human approval are rejected.
5. Path safety: traversal, absolute paths, symlink escape and writes outside the task workspace are rejected.
6. Resource bounds: missing or excessive CPU, memory, time and storage limits are rejected.
7. Evidence: missing, empty, stale or hash-mismatched artifacts cannot satisfy acceptance.
8. Failure handling: failed verification preserves logs and prevents publication.
9. Reproducibility: canonical serialization and plan hash are stable for equivalent source.
10. AI safety: natural-language intent cannot override typed constraints or acceptance rules.

## Explicit non-goals for v1

- Replacing C/Rust/KCC for native applications.
- Executing free-form natural language.
- Arbitrary shell scripts or embedded Python/JavaScript.
- Self-modifying plans during execution.
- Granting capabilities from source declarations.
- Claiming formal verification of the kernel or action handlers.

## Status

Design only. No parser, validator or KIL runtime is claimed to exist yet. Implementation begins only after the v1 grammar, action registry and policy boundary are reviewed and the existing factory can be represented without weakening any current test or release gate.
