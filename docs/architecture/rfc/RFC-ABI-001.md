# RFC-ABI-001 — syscall ABI и Native Application ABI

**Статус:** DRAFT — inventory from main, numbering preserved; calling conventions require architecture source verification.  
**База:** main @ bbc7f7ab948e72511aed113b69043add10ec8350.

## 1. Фактическая точка

- Shared dispatcher: `syscall_handler(number, arg1, arg2, arg3)` in `syscall.c`.
- x86-64 entry configures MSR EFER.SCE, STAR, LSTAR and FMASK in `arch/x86_64/syscall_arch.c`; SYSRET segment selectors have an explicit AMD compatibility correction.
- x86-64 assembly in `arch/x86_64/interrupts.s:325-370`: RAX = syscall number; RDI/RSI/RDX = args 1–3; RAX = result. Entry performs SWAPGS, saves user RSP/RIP/RFLAGS on the per-CPU kernel stack before enabling interrupts, checks canonical return RIP, clears caller-clobbered argument registers before SYSRET, then restores user stack/GS. RCX/R11 are hardware-clobbered; RDI/RSI/RDX/R8-R10 are explicitly treated as clobbered.
- ARM64 SVC path in `arch/aarch64/main.c:user_exception`: ESR exception class 0x15, X8 = syscall number, X0-X2 = args 1–3, result in saved X0. IRQs are enabled around the shared handler and disabled before exception return.
- ARM64 vector frame in `arch/aarch64/boot.S:187-234` saves X0-X30, ELR, SPSR, ESR, FAR and SP_EL0; exception return restores all general registers except X0, which carries the syscall result. The frame also preserves X8 (the syscall number) and restores SP_EL0 after a possible thread switch.
- The C shared handler signature is `syscall_handler(number,arg1,arg2,arg3)`; both architecture adapters currently pass exactly three arguments. AArch64 full exception-frame save/restore and caller-saved register contract still need separate assembly/vector audit.
- `syscall.h` exports init and C handler but does not freeze a public userspace header or ABI version.
- Current process loader uses KIRA-native ELF64 ET_EXEC for the build ISA. This is not a Linux ELF personality.

## 2. Current syscall number inventory (as observed in syscall.c)

| Number | Symbol | Current behavior |
|---:|---|---|
| 1 | SYS_WRITE | process log output |
| 2 | SYS_EXIT | terminate process |
| 3 | SYS_TICKS | monotonic scheduler ticks in 10 ms units |
| 4 | SYS_SLEEP | bounded sleep |
| 5 | SYS_YIELD | yield |
| 6 | SYS_GETPID | process ID |
| 7–9 | SYS_GFX_CREATE/PRESENT/EVENT | graphics surface and events |
| 10 | SYS_FS | file service request |
| 11 | SYS_HEAP | grow process heap |
| 12–14 | SYS_THREAD_CREATE/EXIT/GETTID | thread lifecycle |
| 15–16 | SYS_FUTEX_WAIT/WAKE | futex synchronization |
| 17–19 | SYS_PORT_OPEN/SEND/RECV | named message ports |
| 20–22 | SYS_SHM_OPEN/MAP/UNMAP | shared memory |
| 23 | SYS_CLOSE | close process handle |
| 24 | SYS_CLOCK_NS | monotonic nanoseconds |
| 25–32 | SYS_NET_* | DNS/TCP listen/connect/send/recv/accept |
| 33–35 | SYS_SND_* | audio playback |
| 36 | SYS_RANDOM | random bytes |
| 37 | SYS_TIME | UTC seconds if RTC available |
| 38–41 | SYS_UDP_* | UDP sockets |
| 42–43 | SYS_SND_VOLUME/RECORD | audio volume and capture |
| 44–47 | SYS_NET_CONNECT_SA/UDP_SENDTO/UDP_RECVFROM/NET_PEER | extended socket operations |
| 48–55 | KDRV_SYS_* | driver-specific calls in kdrv_format.h |
| 56 | SYS_JIT | W^X JIT allocation/seal |

This is an inventory, not a promise that all calls have stable cross-release ABI. Preserve current numbers during migration; do not reuse gaps without registry update.

## 3. Required ABI specification

- per-ISA entry register mapping, clobbers, preserved registers and return register (x86 preserves callee-saved registers per C ABI and explicitly clears volatile argument registers; ARM64 exception frame restores X1-X30 unchanged and returns result in X0);
- syscall number namespace and ABI version discovery;
- 64-bit argument layout, alignment, pointer width and structure packing;
- negative error encoding and stable error namespace;
- pointer validation/copy semantics and TOCTOU rules;
- process/thread identity and handle lifecycle;
- capability requirement per call and object-specific rights;
- blocking/cancellation/interrupt/restart behavior;
- time unit, clock source and overflow semantics;
- forward/backward compatibility and deprecation.

## 4. ABI design rule

Keep only privileged mechanisms as direct syscalls. Service-specific operations (filesystem, graphics, audio, networking, device management) should migrate behind capability-checked IPC/service APIs while existing numbers remain compatibility shims until apps are migrated. No number is removed or repurposed without an ABI migration ADR.

## 5. Required tests

- per-ISA entry/return register conformance, including x86 SWAPGS/kernel-stack/canonical-RIP path and ARM64 full X0-X30 frame, SP_EL0 restore, X8 number and X0 result;
- invalid syscall number and malformed argument tests;
- user pointer unmapped/read-only/cross-boundary/concurrently unmapped;
- process killed during blocking syscall;
- syscall from non-user context;
- ABI version mismatch and unknown optional flags;
- cross-target build and same native ABI fixture tests.

## 6. Open decisions

- Exact AArch64 SVC register map and syscall entry assembly.
- Public userspace header location and ABI version handshake.
- Whether current direct service syscalls remain stable forever or are deprecated after IPC equivalents.
- Standard error namespace across native, Linux and Windows compatibility runtimes.
- syscall restart semantics for signals and thread cancellation.
