# KIRA OS — первичная инвентаризация компонентов

**Снимок:** main @ bbc7f7ab948e72511aed113b69043add10ec8350  
**Дата:** 2026-11-13  
**Статус документа:** первичный source-level pass, не финальный аудит.  
**Ограничение:** исходники просмотрены через GitHub; команды сборки и тесты в этом проходе локально не запускались. Наличие тестового файла не считается PASS.

## Правила статусов

- **Код обнаружен — проверка не завершена:** есть реализация/исходники, но этот аудит ещё не подтвердил полный контракт и тесты на точном SHA.
- **Частично:** код покрывает только часть целевой функции или ограниченный формат/API.
- **Не обнаружено в main:** в просмотренной карте main не найдено отдельной реализации. Это не исключает исследовательскую работу в открытых PR.
- **Не проверено:** недостаточно данных; требуется полный просмотр кода и запуск тестов.

## Карта компонентов

| Подсистема | Фактические исходники / точки входа | Предварительный статус | Что доказать/сделать дальше |
|---|---|---|---|
| Kernel bootstrap / init | arch/x86_64/kernel_main.c, arch/x86_64/kernel_boot.c, arch/aarch64/main.c, arch/aarch64/kernel_main.c, kernel.h, bootmap.c, boot/ | x86 entry/boot and ARM64 steady-state loop have separate owners; ARM64 platform bring-up remains in main.c | Проследить порядок init, error unwind, versioned BootInfo, boot contract BIOS/UEFI and ARM64; выполнить clean builds and boot tests |
| x86-64 HAL | arch/x86_64/*, cpu.h, apic.c, gdt.c, idt.c, exceptions.c, smp.c | Код обнаружен — проверка не завершена; F-016 PCI enumeration path не подключён явно | Проверить все IRQ paths, AP startup, PCI mechanism-1 init после security gates, memory ordering, timer, power и аппаратные сценарии |
| ARM64 HAL | arch/aarch64/*, FDT/GIC/PSCI/SMMU paths; arm_main() owns platform discovery/bring-up; kernel_main.c owns steady-state loop; pci_host.c owns FDT ECAM discovery/MMIO/window/BAR setup and passes FDT bus-range to common pci_init_range() | Source ownership partially separated; range-aware enumeration added in architecture follow-up; platform init contract and hardware coverage remain unverified | Отдельно проверить AArch64 build, boot, SMP, GIC, timer, FDT, nonzero ECAM bus-range, BAR, cache/DMA и реальные платы |
| Physical/virtual memory | pmm.c, vmm.c, heap.c, ramext.c, arch/*/vmm* | Код обнаружен — проверка не завершена | Проверить ownership, page-table lifecycle, user isolation, TLB shootdown, OOM, ARM64 parity |
| Scheduler | sched.c, sched.h, arch/*/arch_sched.h, timer.c | Код обнаружен — проверка не завершена | Сверить SMP races, priority/fairness, wake/sleep, stack lifecycle; запустить sched/context tests |
| Process model | proc.c, proc.h | Код обнаружен — проверка не завершена | Проверить process/thread lifecycle, fault teardown, address-space cleanup, handles и resource limits |
| Native ELF loader | elf.c, elf.h, proc.c | **Частично**: parser заявляет ELF64 ET_EXEC своей ISA; динамический linker/shared objects не обнаружены в проверенном пути | Проверить все bounds/relocations/overlaps; определить полноценный KIRA Native ABI и динамическую загрузку |
| Syscall ABI | kernel/syscall.c, syscall.h, syscall_services.h, services/syscall_router.c, services/*_syscall.c, kcc/include/syscall_numbers.h, arch/x86_64/syscall_arch.c, arch/aarch64 syscall path | Код обнаружен — проверка не завершена; numbers centralized and pinned by architecture test | Зафиксировать calling convention, errors, pointer validation и совместимость ABI по ISA |
| IPC / shared memory | kernel/syscall.c (ports/SHM), kobj.c, futex.c, proc.c | Код обнаружен — проверка не завершена | Специфицировать endpoint ownership, peer identity, capability passing, queue limits, timeout, peer crash |
| Capability policy | caps.c, caps.h, proc.c, kernel/syscall.c, services/driver_syscall.c, kdrv.c | **Частично**: статическая таблица полномочий по имени приложения; process caps назначаются при запуске и дальше не меняются | Проверить полноту checks на каждом syscall; спроектировать объектные capabilities, делегирование/отзыв и аудит |
| KIRA Mind | mind.c, mind_sys.c, mindnet.c, mind.h | Код обнаружен — проверка не завершена | Проследить каждое действие и источник телеметрии; проверить policy boundary, диапазоны, rollback и запрет прямого доступа |
| KIRA Mind control boundary | mind_sys.c → sched/proc/pmm/display/storage APIs | **Архитектурный риск для проверки**: в исходниках видны управляющие функции, вызывающие subsystem APIs; требуется доказать единый policy gate и аудит действий | Составить список всех control actions и показать путь request → authorization → apply → audit |
| Driver framework / Manager | kdrv.c, kdrv.h, kdrv_format.h, services/driver_syscall.c, userland/kdrv.h, docs/DRIVERS.md | Код обнаружен — проверка не завершена; F-013/F-014/F-016 security and enumeration blockers in main | KDRV can enable PCI bus mastering for CAP_DMA or CAP_IRQ without a per-device IOMMU domain; pci_init preserves firmware-set bus-master. ARM64 enumerates PCI explicitly; x86 boot does not. PR #136 blocks unisolated Ring 3 DMA/MSI; PR #137 clears firmware bus-master on enumeration. Do not enable x86 enumeration until both security gates and per-device domain lifecycle are verified. Then test lifecycle, revoke, unbind/hotplug, DMA ownership and recovery |
| Driver package trust | kdrv_fmt.c, trust.c, tools/kdrvpack.c, tests/kdrv_test.c, fsrv.c | Код обнаружен — проверка не завершена | Проверить подписи/provenance/anti-rollback на main и активных PR; не путать hash record с подписью издателя |
| KiraFS | kfs.c, kfs.h, tests/kfs_test.c | Код обнаружен — проверка не завершена; v2 описывает crash consistency через write ordering, без journal | Проверить power-cut invariants, metadata/data ordering, corruption recovery, concurrent access и disk-full |
| VFS / removable volumes | vfs.c, vfs.h, fat.c, blk.c | **Частично**: KiraFS root + FAT mounts для внешних томов | Проверить mount lifecycle, path normalization, permissions, hot-unplug и concurrency |
| Block/storage drivers | blk.c, ata.c, ahci.c, nvme.c, virtio_blk.c и device tests | Код обнаружен — проверка не завершена | Проверить DMA, queue lifecycle, timeout/reset, flush/barriers, real hardware |
| Network | net.c, e1000.c, virtio_net.c, rtl8139 legacy path, socket syscalls | Код обнаружен — проверка не завершена | Проверить socket API, protocol coverage, driver lifecycle, malformed packets, network isolation |
| Graphics / window system | display.c, fb.c, wm.c, gfxsrv.c, gfxui.c, apps.c, explorer.c, fm.c, dock.c | Код обнаружен — проверка не завершена | Определить service/process boundaries, compositor contract, input focus, GPU acceleration and crash isolation |
| Audio | audio.c, hda.c, ac97.c, sb16.c, userland audio APIs | Код обнаружен — проверка не завершена | Проверить device lifecycle, buffers, latency, mixing, permissions and recovery |
| WASM Runtime | userland/wasm.c, wasm_jit_x64*, tests/wasm* | **Частично/код обнаружен**: in-process Ring 3 interpreter; комментарий заявляет WASM 1.0 subset, JIT x86-64 | Независимо проверить parser/verifier, bounds, fuel/quotas, JIT W^X, ARM64 interpreter parity, imports and traps |
| KIRA VM | ADR-0001, KVM1 research PR #134 | **Не production в main baseline**: master plan/ADR оставляют WASM production base, KVM1 экспериментальный | Не интегрировать второй ISA без comparative ADR, verifier, compiler, package, ABI and conformance gates |
| Windows app compatibility | Dedicated PE/Win32/NT runtime not identified in main source map | **Не обнаружено в main** | Спецификация PE loader, Win32/NT user-mode ABI, DLL loader, libraries, graphics/audio/network and app matrix |
| Linux app compatibility | Dedicated Linux ELF/syscall runtime not identified; existing ELF loader is KIRA-native | **Не обнаружено в main** | Спецификация Linux syscall ABI, libc/dynamic linker, signals/futex/namespaces and compatibility matrix |
| KIRA Hypervisor | Dedicated VMX/SVM/EL2 guest virtualization subsystem not identified in main source map | **Не обнаружено в main** | Threat model, hardware capability discovery, vCPU/guest memory, exits, virtual devices, IOMMU and guest boot |
| Service Manager | No dedicated service manager implementation identified in first-pass tree/source review | **Не обнаружено / проверить глубже** | Determine whether lifecycle is implicit in kernel init, threads or userland; define manifest, dependency graph, restart/health |
| Event Bus | IPC ports and UI event paths exist; no general system event bus identified | **Не обнаружено как общий контракт** | Decide whether to build on IPC; define schema, ordering, permissions, delivery and backpressure |
| Resource Manager | scale.c, sched.c, pmm.c, mind_sys.c expose resource controls/telemetry | **Распределённые механизмы; единый manager не подтверждён** | Inventory CPU/memory/device/power quotas and ownership; decide service/API boundary |
| Power management | arch/x86_64/power.c and platform-specific code | **Частично/не проверено** | Sleep/resume, ARM64 PSCI, device power states, battery and policy |
| Observability / recovery | uart/logging, watchdog.c, life.c, panic paths, health telemetry | **Частично/не проверено** | Structured logs, tracing, crash dumps, service restart and durable recovery |
| Package/update/installer | package-related sources and build/release scripts; installer/update coverage requires targeted audit | **Не проверено** | Map package format, signatures, atomic update, rollback, boot recovery and user-data preservation |
| KCC/toolchain | kcc/*, tools/*, build scripts, KCC tests | Код обнаружен — проверка не завершена | Verify compiler correctness, IR/backend separation, ABI, reproducibility, x86-64/ARM64 and performance |
| Build/CI/test infrastructure | build_clang.sh, build_arm64.sh, ci/*, tests/run_host_tests.sh, .github/workflows/* | Код обнаружен — тесты в этом проходе не запускались | Run clean host suite, x86/ARM builds, QEMU scenarios and inspect exact workflow results on current SHA |

## Первые архитектурные наблюдения (не окончательные выводы)

1. В kernel.c собран широкий набор прямых include/init-зависимостей. Это может быть допустимым bootstrap composition root, но нужно отделить orchestration от фактических привилегированных обязанностей и проверить направление зависимостей.
2. `kernel/syscall.c` теперь содержит syscall core и не включает заголовки графики/FS/сети/аудио/RTC; service routing находится в `services/syscall_router.c` и шести subsystem adapters (graphics, filesystem, network, audio, clock, Driver Manager). Это source boundary only: адаптеры пока исполняются в kernel context. Следующий gate — typed/versioned requests и IPC migration без изменения Native ABI.
3. proc.c + elf.c сейчас дают базовый KIRA ELF ET_EXEC process path. Это не Linux compatibility и не Windows PE compatibility.
4. caps.c использует статическую таблицу доверенных приложений; это уже полезная least-privilege база, но не полная объектная capability authority с delegation/revocation.
5. KIRA Mind имеет управляющие точки к scheduler/resource/display subsystems. Нужно доказать, что все изменения проходят единую проверку полномочий, ограничений и audit trail.
6. KDRV — отдельный существенный путь Ring 3 driver isolation; его нельзя считать автоматически безопасным без полного DMA/IOMMU, hot-unplug, teardown и hardware evidence.
7. KiraFS v2 заявляет crash consistency через порядок записи без журнала. Нужны power-cut tests, не следует приравнивать отсутствие журнала к отсутствию транзакционной гарантии, пока не проверены инварианты.
8. Наличие vmm.c означает virtual memory manager; это не доказательство существования hardware hypervisor.

## Следующие действия

- Продолжить source review по каждому перечисленному компоненту, добавить точные публичные символы и зависимости.
- Создать dependency map и contract registry, не выдумывая отсутствующие интерфейсы.
- Сопоставить открытые PR с компонентами и точными head SHA; не считать изменения из PR частью main.
- Запустить тесты только в доступном checkout/CI и записать реальные команды, окружение и результат.


**PCI host parsing hardening (architecture branch):** ECAM bus-range and physical-span checks now fail closed; PCI `ranges` decoding is isolated in a host-testable pure parser with signed-offset and overflow validation. This is source/test coverage awaiting exact-head CI, not proof of physical PCI hardware support.


**Latest verified baseline:** CI run `43298087855` on SHA `618ce01344e219991323aca51f9de58c55ed2aab` passed 30/30 jobs, including 16 ARM64 QEMU scenarios. Subsequent bus-range-start and ECAM-alignment hardening is committed but awaits a new exact-head run.


## Device Model contract follow-up (2026-11-13)

- Draft contract: `docs/architecture/rfc/RFC-DEV-001.md` (DEV-001). It defines device identity/generation, resource ownership, driver binding and lifecycle, with explicit DMA/IOMMU and teardown gates.
- Architecture follow-up adds `pci_init_range(first_bus, last_bus)` and bounds recursive bridge traversal; ARM64 now passes the validated FDT bus-range, including nonzero starts. PCI descriptors now carry boot-local monotonic IDs and a scan generation; opaque snapshot handles resolve only within the current enumeration. This remains PCI-only. KDRV now stores `pci_handle_t` and resolves it through `pci_resolve()` before operations; stale handles cause binding quarantine and DMA memory retention rather than stale-pointer use. The common registry, per-device lifecycle, resource reservation, coordinated rescan, bind/unbind and complete teardown remain open.
- Status remains **partial / not accepted** until range-aware enumeration, generation-safe registry, resource reservation, security lifecycle and exact-head integration tests are implemented.
