# 03 — Запланировано / ещё не завершено

Это не список забытых задач, а обязательные направления мастер-плана, которые нельзя потерять при работе над текущим UI или очередной ошибкой CI. Часть пунктов имеет прототипы/документы в ветках, но не завершена как интегрированная production-функция.

| ID | Направление | Критерий завершения |
|---|---|---|
| P-01 | Linux application compatibility runtime | ELF/Linux syscall personality, dynamic loader/libraries, capability-mediated host API, реальное приложение и негативные conformance tests |
| P-02 | Windows application compatibility runtime | PE/COFF loader, Win32/NT API surface, library resolution, реальное приложение и негативные conformance tests |
| P-03 | Аппаратный Hypervisor для целых гостевых ОС | VMX/SVM/EL2, guest memory translation, vCPU/VM-exit lifecycle, virtual devices, IOMMU isolation и guest boot tests |
| P-04 | User-mode system services | Service Manager, Event Bus, Resource/Power Manager, versioned IPC, backpressure, crash/restart и peer-death tests |
| P-05 | Installer / updater / recovery | атомарное обновление, подписанные manifests, rollback/anti-rollback, восстановление после прерванной установки, boot fallback и reproducible update archive |
| P-06 | KIRA Driver Adaptation Layer — полноценная адаптация сторонних драйверов | получение исходников драйвера из разрешённых источников, проверка происхождения/лицензии, адаптер исходного API и KIRA ABI, sandbox build/test, подпись, установка/откат; произвольный бинарный драйвер не объявлять автоматически переносимым |
| P-07 | Физическая аппаратная приёмка | отдельные доказательства на x86-64 и ARM64 устройствах: boot, display/input, storage, network, DMA/IOMMU, power и recovery |
| P-08 | Дополнительные классы устройств | Wi-Fi, Bluetooth, камеры, датчики и аппаратное GPU acceleration с реальной матрицей устройств |
| P-09 | Полный Native runtime / dynamic linking | versioned Native ABI, shared libraries/dynamic linker, relocation validation и cross-ISA conformance |
| P-10 | KIRA Mind — собственный AI-слой ОС с kernel-level интеграцией | bounded typed proposals, policy enforcement владельцами ресурсов ядра, изоляция от прямого произвольного доступа, versioned/signed models, audit/rollback и adversarial tests; наличие mind.c/mind_sys.c само по себе не означает готовый kernel-level AI |

## Не потерять 12 параллельных потоков мастер-плана

1. Kernel + process model + IPC + SMP.
2. KVR + application ABI + native/compatibility runtimes.
3. KCC + IR + backends + build pipeline.
4. Mind Core + specialized models + safe control.
5. Memory safety + IOMMU + secure boot/signatures.
6. Driver framework + device drivers + Driver Manager.
7. KiraFS + storage stack + recovery.
8. Network stack + network services.
9. Desktop/compositor + GUI toolkit + system apps.
10. BIOS/UEFI + installer + update/recovery.
11. x86-64 + ARM64 + hardware abstraction.
12. CI, test infrastructure, fuzzing, performance and release engineering.

## Направления A–N мастер-плана

Архитектура/платформа; ядро; KVR и runtime; KIRA Mind; KCC/toolchain; безопасность памяти/исполнения; драйверы/Driver Manager; KiraFS/storage; сеть; графическая система/desktop; системные службы; boot/installer/update; ISA/реальное оборудование; developer tools/automation/quality.

Любое новое требование сначала привязывается к одному или нескольким направлениям A–N и одному из 12 потоков. Временная работа над блокером не отменяет остальные направления.

## Важные ограничения, которые не превращать в «готово»

- WASM — текущая production-база переносимого KVR; KVM1/KVP0 из PR #132/#134 — эксперимент, не замена WASM.
- Driver Manager/drvget сейчас означает ограниченную установку проверяемых source recipes. Универсальный Linux/Windows binary/ABI translator и безопасная автоматическая адаптация чужих драйверов не завершены.
- Наличие vmm.c не означает аппаратную виртуализацию гостевых ОС.
- Наличие кода ARM64 и успешной QEMU-сборки не означает поддержку физических ARM64 устройств.
- Наличие GUI-компонента не означает завершённую Spatial визуальную приёмку.
- Наличие KIRA Mind в ядре не означает, что завершены model lifecycle, policy authority и безопасная автономность.
