# KIRA OS — автоматическая инвентаризация и базовый аудит

> Это автоматический инвентаризационный отчёт, а не заявление о полном доказательстве корректности ядра. Смысловые выводы требуют проверки исходников и поведения на стенде.

## Состав репозитория

- Отслеживаемых файлов: **852**
- Файлов C: **600**; заголовков: **168**
- Файлов в tests/: **443**
- Workflow GitHub Actions: **2**
- Сценариев x86_64 в CI: **40**
- Сценариев ARM64 в runner: **16**
- Git submodules: **0**; .gitmodules: **нет**

## Проверки синтаксиса

- Bash: ошибок нет
- Python AST: ошибок нет
- C/ASM компиляция и исполняемые проверки остаются в существующих host-tests/build/QEMU jobs.

## Внешние инструменты и зависимости

| Инструмент | Роль в сборке/проверках |
|---|---|
| clang | host/kernel and userland compiler |
| ld.lld | linker |
| nasm | x86 assembler |
| grub-mkrescue | x86 ISO bootloader/image creation |
| xorriso | ISO image tooling |
| llvm-objcopy | ARM64 image tooling |
| wasm-ld | WebAssembly linker |
| qemu-system-x86 | x86_64 integration testing |
| qemu-system-arm | ARM64 integration testing |
| gcc | host tests and helper tools |
| python3 | CI/test orchestration |
| openssl | TLS interoperability tests |
| dosfstools | FAT test images and validation |
| mtools | FAT test images and validation |
| python3-cryptography | certificate/crypto test oracle |
| python3-pil | image fixture and screenshot tests |
| KVM | accelerated x86_64 QEMU tests |

Классификация: Clang/LLD/NASM/wasm-ld и GRUB относятся к сборке; QEMU/KVM, GCC, OpenSSL, Python-модули, dosfstools и mtools — к тестированию и стенду. GRUB попадает в x86_64 ISO и потому остаётся внешней runtime-зависимостью загрузки. Наличие этих инструментов в CI не означает, что они входят в запущенное ядро.

## Известные архитектурные ограничения

- x86_64 ISO creation currently depends on GRUB/Multiboot2; no independent KIRA BIOS/UEFI loader was found in the tracked tree.
- The current build still uses external Clang/LLD/NASM (and wasm-ld); kcc is a limited in-OS C compiler, not yet the kernel's complete self-hosting toolchain.
- drvget accepts selected BSD-style source recipes and rejects Linux .ko and Windows .sys binaries; general Linux driver translation remains future work.
- Driver package records use explicit user confirmation and SHA-256; project progress notes say package signatures are absent. A hash is integrity evidence, not publisher authentication.
- The driver UI warns that there is no IOMMU; DMA-capable device isolation is therefore incomplete.
- ARM64 and x86_64 have QEMU coverage, but physical boards and real PCI/USB/network hardware have not been validated by these CI runs.
- The ARM64 non-coherent-DMA scenario is a QEMU model; the physical non-coherent hardware path remains unverified.
- ACPI AML _PRT routing is listed as unfinished; some legacy INTx routing remains incomplete.
- docs/HANDOFF.md is stale relative to the current CI matrix and docs/PROGRESS.md.
- ARCHITECTURE.md contains historical status text that contradicts the current QEMU CI coverage; docs need one authoritative current status.

## Поиск маркеров незавершённой реализации

Найдено совпадений-маркеров в производственных исходниках: **0**. Это эвристика: совпадение требует ручной оценки и само по себе не доказывает наличие незавершённой реализации.

## Ограничения этого отчёта

- Список инструментов извлекается из основных build/CI scripts и не заменяет анализ каждого include/import и всех linker inputs.
- Поиск маркеров не способен доказать отсутствие пустых функций, неверной логики, гонок или скрытых незавершённых реализаций.
- Успешные QEMU-прогоны не подтверждают совместимость с физическим оборудованием.
- Точные результаты текущего запуска находятся в TEST_REPORT.md, который формируется отдельно после завершения всех jobs.
