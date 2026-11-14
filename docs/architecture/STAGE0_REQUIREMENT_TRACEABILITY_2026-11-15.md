# KIRA OS — requirement traceability ledger (Stage 0)

**Источник истины требований:** `docs/MASTER_DEVELOPMENT_PLAN.md`  
**Актуальный Stage:** 0 — IN PROGRESS.  
**Статус этого реестра:** полная трассировка исходных требований по разделам A–N, параллельным потокам, историческим этапам, release requirements и технологической политике. Это не подтверждение реализации.

## Назначение

Этот ledger нужен, чтобы ни один ИИ/разработчик не потерял требование при переходе на новую карту Stage 0–13. Каждая строка сохраняет исходное требование, ссылку на раздел и строку мастер-плана, целевые Stage и статус доказательства.

- Всего записей: **233**.
- Направления A–N, включая вложенные пункты: **135**.
- Параллельные потоки: **13**.
- Исторические stage criteria: **35**.
- Release requirements: **6**.
- Технологическая/архитектурная политика §8: **32**.

## Правила чтения

1. `NOT VERIFIED` означает, что требование сохранено, но его production-реализация и тестовое доказательство ещё не закрыты.
2. `TRACKED` означает сохранённый рабочий поток, а не успешную реализацию.
3. Целевые Stage — назначение требования для планирования; одно требование может иметь несколько зависимостей.
4. Исторические Stage раздела 4 не удаляются, но новая работа интегрируется через Stage 0–13.
5. Изменение или снятие требования требует ADR/явного решения; строку нельзя удалять молча.
6. Для подтверждения реализации в следующих итерациях к каждой записи добавлять component/file/build/test evidence, exact SHA и среду.

## Полная трассировка

| ID | Источник мастер-плана | Stage | Исходное требование | Состояние |
|---|---|---:|---|---|
| A-01 | §2 A / L37 | 0–5 | Зафиксировать спецификацию архитектуры: уровни привилегий, адресные пространства, ABI/API, формат модулей, модель объектов и ресурсов, загрузка, безопасность и совместимость. | NOT VERIFIED |
| A-02 | §2 A / L38 | 0–5 | Разделить ядро, серверы/системные службы, runtime и приложения. | NOT VERIFIED |
| A-03 | §2 A / L39 | 0–5 | Ввести стабильные внутренние контракты, версионирование ABI и проверку совместимости. | NOT VERIFIED |
| A-04 | §2 A / L40 | 0–5 | Поддержать диагностику, трассировку, журнал событий и управляемое восстановление. | NOT VERIFIED |
| A-05 | §2 A / L41 | 0–5 | Устранить архитектурные зависимости, которые вынуждают верхние уровни знать детали конкретного CPU или контроллера. | NOT VERIFIED |
| B-01 | §2 B / L44 | 2–4 | Загрузка и ранняя инициализация; GDT/IDT, исключения, IRQ, APIC/GIC, таймеры. | NOT VERIFIED |
| B-02 | §2 B / L45 | 2–4 | PMM, VMM, отображение страниц, higher-half, W^X, NX, guard pages, copy-on-write при необходимости. | NOT VERIFIED |
| B-03 | §2 B / L46 | 2–4 | Процессы и потоки, контексты CPU, вытесняющий планировщик, SMP/per-CPU очереди, affinity и синхронизация. | NOT VERIFIED |
| B-04 | §2 B / L47 | 2–4 | Syscall ABI, проверка пользовательских указателей, IPC, события, сигналы/уведомления, таймеры. | NOT VERIFIED |
| B-05 | §2 B / L48 | 2–4 | Управление ресурсами, завершение и очистка процессов, обработка ошибок, watchdog и восстановление. | NOT VERIFIED |
| B-06 | §2 B / L49 | 2–4 | Защита ядра: SMEP/SMAP или архитектурные аналоги, KASLR при обоснованной готовности, stack canaries, контроль целостности. | NOT VERIFIED |
| B-07 | §2 B / L50 | 2–4 | Никаких незащищённых обходов из user mode в kernel mode. | NOT VERIFIED |
| C-01 | §2 C / L56 | 9–10, 13 | В репозитории уже есть пользовательский WebAssembly runtime: `userland/wasm.c`, запускаемый как Ring 3/EL0 процесс `wasm`; он загружает модуль через grant-файл, ограничивает линейную память, предоставляет ограниченный импортный API KIRA и выполняет байткод внутри отдельного процесса. | NOT VERIFIED |
| C-02 | §2 C / L57 | 9–10, 13 | Для x86-64 уже есть JIT (`userland/wasm_jit_x64.h`, `userland/wasm_jit_x64_gen.h`) с выделением RW-NX страниц и однократным sealing в R-X через `SYS_JIT`/ `CAP_JIT`; неподдерживаемая JIT-команда должна оставаться на интерпретаторном пути. | NOT VERIFIED |
| C-03 | §2 C / L58 | 9–10, 13 | ARM64 использует интерпретатор и кванты выполнения; x86-64 использует JIT при успешной генерации. Нельзя считать JIT доступным на ARM64. | NOT VERIFIED |
| C-04 | §2 C / L59 | 9–10, 13 | Модули и примеры размещаются в `/wasm`; сборка модулей — `userland/wasm/build.sh`; текущие проверки — `tests/wasm_host.c`, `tests/wasm_diff.sh`, `tests/wasm_gen.py` и CI-сценарии `apps-wasm`/ `wasm-speed`. | NOT VERIFIED |
| C-05 | §2 C / L60 | 9–10, 13 | Поэтому базовый переносимый формат KVR — WebAssembly, а не новый несовместимый самодельный bytecode. Не создавать второй VM ISA без отдельного архитектурного решения с измеримыми преимуществами и миграционным планом. | NOT VERIFIED |
| C-06 | §2 C / L61 | 9–10, 13 | Текущая реализация является рабочей базой и ограниченным подмножеством WebAssembly 1.0, но НЕ эквивалентна полной современной WASM-совместимости и НЕ считается завершённой системой виртуализации приложений. | NOT VERIFIED |
| C-07 | §2 C / L62 | 9–10, 13 | Перед расширением KVR провести независимый аудит парсера/верификатора, загрузчика секций, проверки типов и индексов, imports/exports, JIT-контракта, квот/прерывания выполнения и host API. Не объявлять безопасность по факту изоляции процесса без негативных тестов на ошибки самого runtime. | NOT VERIFIED |
| C-08 | §2 C / L66 | 9–10, 13 | Собственный байткод/ISA виртуальной машины и версионированный формат модулей. | NOT VERIFIED |
| C-09 | §2 C / L67 | 9–10, 13 | Проверяемый загрузчик модулей, верификатор байткода, типовая/контрольная безопасность, проверка границ памяти и переходов. | NOT VERIFIED |
| C-10 | §2 C / L68 | 9–10, 13 | Изолированная память, стек, таблица ресурсов и capabilities для каждого экземпляра. | NOT VERIFIED |
| C-11 | §2 C / L69 | 9–10, 13 | Интерпретатор как первая проверяемая реализация; JIT — отдельный этап с W^X, контролем генерации кода и fallback на интерпретатор. | NOT VERIFIED |
| C-12 | §2 C / L70 | 9–10, 13 | Host API через узкие capability-интерфейсы: файлы, сеть, графика, звук, ввод, таймеры, процессы и устройства. | NOT VERIFIED |
| C-13 | §2 C / L71 | 9–10, 13 | Квоты CPU/памяти/дескрипторов, отмена, пауза/возобновление, watchdog, диагностика и безопасное завершение. | NOT VERIFIED |
| C-14 | §2 C / L72 | 9–10, 13 | Пакеты приложений, подпись/доверие, версия ABI и атомарное обновление. | NOT VERIFIED |
| C-15 | §2 C / L73 | 9–10, 13 | Один переносимый модуль должен работать на x86-64 и ARM64 без перекомпиляции, если использует переносимый KIRA ABI. | NOT VERIFIED |
| C-16 | §2 C / L74 | 9–10, 13 | Архитектурное решение ADR-0001: в текущем release baseline основная KVR — существующая WASM VM; KVM1 на draft PR #134 остаётся исследовательским прототипом, его ABI/runner/build integration не замораживаются и не сливаются до сравнительных измерений и отдельного review. | NOT VERIFIED |
| C-17 | §2 C / L77 | 9–10, 13 | Запуск нативных приложений x86-64 и AArch64 с единым системным ABI, загрузчиком, проверкой ELF/релокаций и изоляцией процессов. | NOT VERIFIED |
| C-18 | §2 C / L78 | 9–10, 13 | Контролируемые мосты между native-процессами и KIRA VM. | NOT VERIFIED |
| C-19 | §2 C / L79 | 9–10, 13 | Управление библиотеками, зависимостями, версиями ABI и правами. | NOT VERIFIED |
| C-20 | §2 C / L82 | 9–10, 13 | Отдельные изолированные среды совместимости для приложений сторонних ОС. | NOT VERIFIED |
| C-21 | §2 C / L83 | 9–10, 13 | Слой адаптации API/ABI, форматов исполняемых файлов и системных вызовов только в рамках документированной совместимости. | NOT VERIFIED |
| C-22 | §2 C / L84 | 9–10, 13 | Никакого обещания «запуска любого приложения» без подтверждённой матрицы совместимости. | NOT VERIFIED |
| C-23 | §2 C / L85 | 9–10, 13 | Совместимость не должна ослаблять ядро или выдавать гостю прямой доступ к устройствам. | NOT VERIFIED |
| C-24 | §2 C / L88 | 9–10, 13 | Отдельный гипервизорный контур для запуска целых гостевых ОС при наличии VT-x/AMD-V или ARM virtualization extensions. | NOT VERIFIED |
| C-25 | §2 C / L89 | 9–10, 13 | Управление VM, vCPU, гостевой памятью, виртуальными устройствами, snapshot/restore и изоляцией DMA. | NOT VERIFIED |
| C-26 | §2 C / L90 | 9–10, 13 | Это отдельная функция от KIRA VM; не делать её обязательной для запуска обычных приложений. | NOT VERIFIED |
| C-27 | §2 C / L93 | 9–10, 13 | Фаззинг верификатора и загрузчика, malformed bytecode, escape/изоляционные тесты. | NOT VERIFIED |
| C-28 | §2 C / L94 | 9–10, 13 | Тесты resource exhaustion, гонок, отмены и восстановления. | NOT VERIFIED |
| C-29 | §2 C / L95 | 9–10, 13 | Совместимость байткода между x86-64 и ARM64. | NOT VERIFIED |
| C-30 | §2 C / L96 | 9–10, 13 | Негативные тесты на обход capabilities, W^X и sandbox boundary. | NOT VERIFIED |
| D-01 | §2 D / L99 | 11 | Mind Core, сбор и нормализация телеметрии, модели подсистем, хранилище состояния и памяти. | NOT VERIFIED |
| D-02 | §2 D / L100 | 11 | Прогнозирование давления памяти, нагрузки CPU, деградации устройств и вероятных отказов. | NOT VERIFIED |
| D-03 | §2 D / L101 | 11 | Диагностика причин с объяснениями и привязкой к наблюдаемым данным. | NOT VERIFIED |
| D-04 | §2 D / L102 | 11 | Безопасная оптимизация расписания и ресурсов с ограничениями и возможностью отката. | NOT VERIFIED |
| D-05 | §2 D / L103 | 11 | Обучение/адаптация на локальной машине, контроль качества моделей, версионирование, подпись и rollback. | NOT VERIFIED |
| D-06 | §2 D / L104 | 11 | Плагины/специализированные модели для памяти, планировщика, накопителей, сети, питания, драйверов и приложений. | NOT VERIFIED |
| D-07 | §2 D / L105 | 11 | Mind не получает безусловных привилегий: опасные действия проходят через policy engine, capabilities и аудит. | NOT VERIFIED |
| D-08 | §2 D / L106 | 11 | Не допускать гонок, неограниченного роста очередей и блокировки критических путей ядра. | NOT VERIFIED |
| E-01 | §2 E / L109 | 9 | Спецификация KIRA IR, SSA, типов, calling convention, debug info и ABI. | NOT VERIFIED |
| E-02 | §2 E / L110 | 9 | Фронтенд языка/подмножества C, диагностика, линковка и форматы объектных файлов. | NOT VERIFIED |
| E-03 | §2 E / L111 | 9 | Backend x86-64 и AArch64, relocations, ELF и оптимизации. | NOT VERIFIED |
| E-04 | §2 E / L112 | 9 | Проверки безопасности памяти и ABI до генерации машинного кода. | NOT VERIFIED |
| E-05 | §2 E / L113 | 9 | Оптимизатор, межпроцедурный анализ, LTO/PGO при измеримой пользе. | NOT VERIFIED |
| E-06 | §2 E / L114 | 9 | Изоляция контекстов компилятора, потокобезопасность и параллельная сборка. | NOT VERIFIED |
| E-07 | §2 E / L115 | 9 | Интеграция KIRA Mind → KIRA IR → KCC Optimizer → Memory Safety Engine → Build & Verification. | NOT VERIFIED |
| E-08 | §2 E / L116 | 9 | Self-hosting как отдельная цель с bootstrap-компилятором и воспроизводимой проверкой. | NOT VERIFIED |
| E-09 | §2 E / L117 | 9 | Сборка ядра, runtime, драйверов и пользовательских приложений одним контролируемым toolchain. | NOT VERIFIED |
| F-01 | §2 F / L120 | 3, 5–7, 10, 13 | Проверка указателей, границ, lifetime и ownership на границах API. | NOT VERIFIED |
| F-02 | §2 F / L121 | 3, 5–7, 10, 13 | Изоляция user/kernel, процессов, KIRA VM и драйверов. | NOT VERIFIED |
| F-03 | §2 F / L122 | 3, 5–7, 10, 13 | W^X, NX, защита таблиц страниц, stack/heap guards, проверка релокаций. | NOT VERIFIED |
| F-04 | §2 F / L123 | 3, 5–7, 10, 13 | Безопасный обмен буферами и shared memory с явным владением и синхронизацией. | NOT VERIFIED |
| F-05 | §2 F / L124 | 3, 5–7, 10, 13 | DMA API с pin/unpin, cache coherency, IOMMU-доменами и безопасным teardown. | NOT VERIFIED |
| F-06 | §2 F / L125 | 3, 5–7, 10, 13 | VT-d и ARM SMMUv3/SMMU: корректные таблицы, invalidation, fault handling, quarantine. | NOT VERIFIED |
| F-07 | §2 F / L126 | 3, 5–7, 10, 13 | Secure boot, подпись ядра/модулей/драйверов/пакетов, ротация ключей, anti-rollback. | NOT VERIFIED |
| F-08 | §2 F / L127 | 3, 5–7, 10, 13 | Тесты на privilege escalation, use-after-free, double-free, integer overflow, TOCTOU и DMA bypass. | NOT VERIFIED |
| G-01 | §2 G / L130 | 6 | Единая модель устройств, discovery, перечисление шин, power/resource lifecycle. | NOT VERIFIED |
| G-02 | §2 G / L131 | 6 | PCI/PCIe, ACPI, FDT, MMIO, IRQ/MSI/MSI-X, DMA и IOMMU. | NOT VERIFIED |
| G-03 | §2 G / L132 | 6 | USB UHCI/OHCI/EHCI/xHCI, USB hubs, HID, накопители и hotplug. | NOT VERIFIED |
| G-04 | §2 G / L133 | 6 | Storage: virtio-blk, ATA, AHCI, NVMe; очереди, таймауты, reset и восстановление. | NOT VERIFIED |
| G-05 | §2 G / L134 | 6 | Audio: AC97, HDA и общий аудио API. | NOT VERIFIED |
| G-06 | §2 G / L135 | 6 | Network: virtio-net и реальные сетевые адаптеры; Wi-Fi как отдельная последующая ветка. | NOT VERIFIED |
| G-07 | §2 G / L136 | 6 | Display/GPU: framebuffer/RAMFB сначала, затем аппаратные GPU и ускорение. | NOT VERIFIED |
| G-08 | §2 G / L137 | 6 | Ввод, камеры, Bluetooth, питание, датчики и другие классы по мере появления hardware support. | NOT VERIFIED |
| G-09 | §2 G / L138 | 6 | Отдельный GUI Driver Manager: обнаружение устройства, поиск каталога, получение исходника/пакета, анализ происхождения и лицензии, проверка ABI, адаптация/пересборка, подпись, тестирование в sandbox, установка/обновление/откат/удаление. | NOT VERIFIED |
| G-10 | §2 G / L139 | 6 | Не загружать и не исполнять неизвестный бинарный драйвер из сети напрямую. Любая адаптация должна иметь проверяемый исходник/происхождение, лицензионный контроль, sandbox и подпись. | NOT VERIFIED |
| G-11 | §2 G / L140 | 6 | Не считать «переписывание драйвера другой ОС» автоматической гарантией совместимости: поддерживать явные адаптеры и матрицу подтверждённых устройств. | NOT VERIFIED |
| H-01 | §2 H / L143 | 7 | Файлы/каталоги, метаданные, права, ACL/capabilities, ссылки и атомарные операции. | NOT VERIFIED |
| H-02 | §2 H / L144 | 7 | Журналирование или copy-on-write транзакции, барьеры записи и crash consistency. | NOT VERIFIED |
| H-03 | §2 H / L145 | 7 | fsck/recovery, контроль целостности, обнаружение повреждений и резервные метаданные. | NOT VERIFIED |
| H-04 | §2 H / L146 | 7 | Кэширование, writeback, concurrent access и блокировки. | NOT VERIFIED |
| H-05 | §2 H / L147 | 7 | Разделы, тома, монтирование, removable media и файловые API для VM/native apps. | NOT VERIFIED |
| H-06 | §2 H / L148 | 7 | Шифрование данных и ключевое хранилище — после проектирования модели угроз. | NOT VERIFIED |
| H-07 | §2 H / L149 | 7 | Фаззинг парсера, power-cut тесты, стресс, долгосрочные тесты и миграции формата. | NOT VERIFIED |
| I-01 | §2 I / L152 | 6, 8 | Ethernet device API, сетевые буферы и DMA-safe путь. | NOT VERIFIED |
| I-02 | §2 I / L153 | 6, 8 | ARP/ND, IPv4/IPv6, ICMP, UDP, TCP, DNS, DHCP. | NOT VERIFIED |
| I-03 | §2 I / L154 | 6, 8 | TLS/криптографические API и безопасное хранение сертификатов/ключей. | NOT VERIFIED |
| I-04 | §2 I / L155 | 6, 8 | Сокеты и высокоуровневый сетевой API для системных служб, VM и приложений. | NOT VERIFIED |
| I-05 | §2 I / L156 | 6, 8 | Firewall/policy, сетевые capabilities, изоляция сетевых пространств. | NOT VERIFIED |
| I-06 | §2 I / L157 | 6, 8 | Диагностика, статистика, восстановление соединения и тесты протоколов. | NOT VERIFIED |
| J-01 | §2 J / L160 | 6, 12 | Display abstraction, framebuffer, compositor, damage tracking, оконный менеджер. | NOT VERIFIED |
| J-02 | §2 J / L161 | 6, 12 | Desktop, Dock, панели, launcher, управление окнами, рабочие пространства. | NOT VERIFIED |
| J-03 | §2 J / L162 | 6, 12 | GUI toolkit и стабильный API графики/ввода. | NOT VERIFIED |
| J-04 | §2 J / L163 | 6, 12 | Файловый менеджер, настройки, терминал, системный монитор, уведомления, диспетчер задач. | NOT VERIFIED |
| J-05 | §2 J / L164 | 6, 12 | Кириллица/Unicode, шрифты, масштабирование, разные разрешения и DPI. | NOT VERIFIED |
| J-06 | §2 J / L165 | 6, 12 | Аудио/мультимедиа, clipboard, drag-and-drop и accessibility. | NOT VERIFIED |
| J-07 | §2 J / L166 | 6, 12 | Интеграция Mind: наблюдение и понятные объяснения, но не навязчивое управление. | NOT VERIFIED |
| J-08 | §2 J / L167 | 6, 12 | Изоляция GUI-приложений и поверхности отрисовки между процессами. | NOT VERIFIED |
| J-09 | §2 J / L170 | 6, 12 | Чистое рабочее пространство с большим свободным центральным полем; не копировать модель Windows/macOS/Linux. | NOT VERIFIED |
| J-10 | §2 J / L171 | 6, 12 | Не использовать постоянно раскрытое меню запуска, классическую панель задач, россыпь ярлыков или постоянные панели телеметрии. | NOT VERIFIED |
| J-11 | §2 J / L172 | 6, 12 | Постоянный слой ограничить компактной навигацией, небольшим Dock, системной зоной статуса/времени и переключателем рабочих пространств. | NOT VERIFIED |
| J-12 | §2 J / L173 | 6, 12 | KIRA AI доступен быстрым вызовом; развёрнутая панель появляется по запросу, а не занимает постоянную часть экрана. | NOT VERIFIED |
| J-13 | §2 J / L174 | 6, 12 | CPU/RAM/диск, графики, журнал служб, диагностика, диспетчер задач и история уведомлений — контекстные поверхности. | NOT VERIFIED |
| J-14 | §2 J / L175 | 6, 12 | Визуальный язык: глубокая сине-фиолетовая палитра, сдержанные эффекты, цельные мягкие формы, авторские иконки, читаемая кириллица, функциональная анимация с reduced-motion. | NOT VERIFIED |
| J-15 | §2 J / L176 | 6, 12 | Адаптация к DPI/разрешениям и мыши/клавиатуре/сенсорному вводу; доступность и контраст не зависят от обоев/прозрачности. | NOT VERIFIED |
| J-16 | §2 J / L177 | 6, 12 | Полная спецификация и этапы UI-0…UI-5: `docs/KIRA_DESIGN_SYSTEM.md`. | NOT VERIFIED |
| J-17 | §2 J / L178 | 6, 12 | Это утверждённое направление дизайна, не заявление о завершённой реализации. Приоритет реализации: сначала аудит/инвентаризация текущей GUI, затем токены и оболочка, затем контекстные поверхности и регрессионные тесты. | NOT VERIFIED |
| K-01 | §2 K / L181 | 5, 8, 10, 12 | Init/service manager, IPC, capabilities, lifecycle и зависимости служб. | NOT VERIFIED |
| K-02 | §2 K / L182 | 5, 8, 10, 12 | Единый KIRA System API для файлов, процессов, сети, устройств, графики, звука и уведомлений. | NOT VERIFIED |
| K-03 | §2 K / L183 | 5, 8, 10, 12 | Пакетный менеджер, репозиторий, подпись пакетов, зависимости, обновление и rollback. | NOT VERIFIED |
| K-04 | §2 K / L184 | 5, 8, 10, 12 | SDK, документация, примеры и инструменты диагностики разработчика. | NOT VERIFIED |
| K-05 | §2 K / L185 | 5, 8, 10, 12 | Приложения как KIRA VM modules и native applications; понятный выбор runtime. | NOT VERIFIED |
| K-06 | §2 K / L186 | 5, 8, 10, 12 | Системные приложения и интеграция с Driver Manager и Mind. | NOT VERIFIED |
| L-01 | §2 L / L189 | 1, 7, 13 | Собственная цепочка загрузки для BIOS и UEFI, не зависящая от Linux. | NOT VERIFIED |
| L-02 | §2 L / L190 | 1, 7, 13 | MBR/GPT, EFI System Partition, boot manager, boot entries и безопасная передача boot info. | NOT VERIFIED |
| L-03 | §2 L / L191 | 1, 7, 13 | Проверка образов, модулей, карты памяти, адресов и параметров загрузки. | NOT VERIFIED |
| L-04 | §2 L / L192 | 1, 7, 13 | Графический установщик, разметка дисков, выбор томов, установка загрузчика и конфигурации. | NOT VERIFIED |
| L-05 | §2 L / L193 | 1, 7, 13 | Обновления A/B или другой атомарный механизм, проверка целостности, rollback и anti-rollback. | NOT VERIFIED |
| L-06 | §2 L / L194 | 1, 7, 13 | Recovery mode, диагностика неудачной загрузки и безопасное восстановление. | NOT VERIFIED |
| L-07 | §2 L / L195 | 1, 7, 13 | ISO для Limbo/QEMU и отдельные образы для реального x86-64 и ARM64 железа. | NOT VERIFIED |
| M-01 | §2 M / L198 | 1–6, 13 | x86-64: BIOS/UEFI, ACPI, APIC, SMP, PCIe, VT-x/AMD-V, VT-d. | NOT VERIFIED |
| M-02 | §2 M / L199 | 1–6, 13 | ARM64: EL уровни, GICv2/v3, FDT/ACPI, PSCI, timers, SMP, SMMUv3, SVE/NEON по возможностям. | NOT VERIFIED |
| M-03 | §2 M / L200 | 1–6, 13 | Общий HAL и проверка архитектурных инвариантов. | NOT VERIFIED |
| M-04 | §2 M / L201 | 1–6, 13 | QEMU/OVMF/virt машины как ранний стенд, Limbo как пользовательский тестовый стенд. | NOT VERIFIED |
| M-05 | §2 M / L202 | 1–6, 13 | Матрица реального железа: CPU, платы, контроллеры, GPU, сеть, USB, аудио, накопители. | NOT VERIFIED |
| M-06 | §2 M / L203 | 1–6, 13 | Не заявлять поддержку физического устройства на основании одного эмулятора. | NOT VERIFIED |
| N-01 | §2 N / L206 | 0–13 | Повторяемая сборка, фиксированные версии инструментов, кеширование и артефакты. | NOT VERIFIED |
| N-02 | §2 N / L207 | 0–13 | Host tests, unit/integration tests, QEMU x86-64/ARM64, UEFI/BIOS, Limbo-совместимость. | NOT VERIFIED |
| N-03 | §2 N / L208 | 0–13 | Фаззинг, sanitizers где применимо, статический анализ, аудит зависимостей и лицензий. | NOT VERIFIED |
| N-04 | §2 N / L209 | 0–13 | Регрессии производительности, стресс, fault injection, power-cut и долгие прогоны. | NOT VERIFIED |
| N-05 | §2 N / L210 | 0–13 | Проверка PR по затронутым подсистемам и межподсистемным контрактам. | NOT VERIFIED |
| N-06 | §2 N / L211 | 0–13 | Независимая проверка изменений; тестовый отчёт содержит команду, окружение, commit SHA и реальный результат. | NOT VERIFIED |
| N-07 | §2 N / L212 | 0–13 | Артефакты релиза: ISO, образы, обновление, manifest, SHA-256, release notes и Termux-инструкция. | NOT VERIFIED |
| N-08 | §2 N / L213 | 0–13 | Автоматизация не должна менять код/сливать PR без заданных прав и правил. | NOT VERIFIED |
| W-01 | §3 L219 | 2–5 | Kernel + process model + IPC + SMP. | TRACKED |
| W-02 | §3 L220 | 9–10 | KVR + application ABI + native/compatibility runtimes. | TRACKED |
| W-03 | §3 L221 | 9 | KCC + IR + backends + build pipeline. | TRACKED |
| W-04 | §3 L222 | 11 | Mind Core + specialized models + safe control. | TRACKED |
| W-05 | §3 L223 | 3, 6, 13 | Memory safety + IOMMU + secure boot/signatures. | TRACKED |
| W-06 | §3 L224 | 6 | Driver framework + device drivers + Driver Manager. | TRACKED |
| W-07 | §3 L225 | 7 | KiraFS + storage stack + recovery. | TRACKED |
| W-08 | §3 L226 | 8 | Network stack + network services. | TRACKED |
| W-09 | §3 L227 | 12 | Desktop/compositor + GUI toolkit + system apps. | TRACKED |
| W-10 | §3 L228 | 1, 13 | BIOS/UEFI + installer + update/recovery. | TRACKED |
| W-11 | §3 L229 | 1–6, 13 | x86-64 + ARM64 + hardware abstraction. | TRACKED |
| W-12 | §3 L230 | 0–13 | CI, test infrastructure, fuzzing, performance and release engineering. | TRACKED |
| W-RULE | §3 L232 | 0–13 | Параллельность означает согласованную работу над разными файлами/модулями с явными границами. При конфликте сначала согласовать интерфейсы, не затирать чужие изменения и не плодить конкурирующие реализации. | TRACKED |
| LEGACY-STAGE-0 | §4 L238 | 0 | Исторический gate: архитектурная фиксация и инвентаризация | PRESERVED / MAPPED |
| LEGACY-0-01 | §4 L239 | 0 | Сверить код с этим планом, составить карту реализовано/частично/отсутствует/не проверено. | NOT VERIFIED |
| LEGACY-0-02 | §4 L240 | 0 | Зафиксировать публичные ABI и зависимости. | NOT VERIFIED |
| LEGACY-0-03 | §4 L241 | 0 | Разделить доказанные возможности и заявленные намерения. | NOT VERIFIED |
| LEGACY-STAGE-1 | §4 L243 | 1 | Исторический gate: надёжная базовая система | PRESERVED / MAPPED |
| LEGACY-1-01 | §4 L244 | 1–4 | Стабильная загрузка BIOS/UEFI, исключения/IRQ, память, процессы, syscalls, KiraFS и консоль/GUI. | NOT VERIFIED |
| LEGACY-1-02 | §4 L245 | 1–4 | Чистая сборка и автоматические регрессии на x86-64. | NOT VERIFIED |
| LEGACY-STAGE-2 | §4 L247 | 0–13 | Исторический gate: изоляция и многозадачность | PRESERVED / MAPPED |
| LEGACY-2-01 | §4 L248 | 2–6 | SMP, полноценный IPC, resource lifecycle, безопасные user/kernel границы, DMA/IOMMU. | NOT VERIFIED |
| LEGACY-2-02 | §4 L249 | 2–6 | Драйверы не могут повредить память ядра или других процессов. | NOT VERIFIED |
| LEGACY-STAGE-3 | §4 L251 | 0–13 | Исторический gate: исполняемая платформа | PRESERVED / MAPPED |
| LEGACY-3-01 | §4 L252 | 9–10 | KIRA VM спецификация, verifier, runtime, host API и пакетный формат. | NOT VERIFIED |
| LEGACY-3-02 | §4 L253 | 9–10 | Native runtime с единым ABI; мосты между runtime и системными службами. | NOT VERIFIED |
| LEGACY-3-03 | §4 L254 | 9–10 | Один и тот же VM-пакет проверен на x86-64 и ARM64. | NOT VERIFIED |
| LEGACY-STAGE-4 | §4 L256 | 0–13 | Исторический gate: инструментарий и интеллект | PRESERVED / MAPPED |
| LEGACY-4-01 | §4 L257 | 9, 11 | KCC/IR/backends, memory safety pipeline, воспроизводимая сборка и начальный self-hosting. | NOT VERIFIED |
| LEGACY-4-02 | §4 L258 | 9, 11 | Mind Core использует реальные метрики и только ограниченные, проверяемые управляющие действия. | NOT VERIFIED |
| LEGACY-STAGE-5 | §4 L260 | 0–13 | Исторический gate: полноценные устройства и пользовательская среда | PRESERVED / MAPPED |
| LEGACY-5-01 | §4 L261 | 6–8, 12–13 | Драйверы, сеть, аудио, графика, файловый менеджер, настройки, системные приложения и Driver Manager. | NOT VERIFIED |
| LEGACY-5-02 | §4 L262 | 6–8, 12–13 | Установка/обновление на диске и recovery. | NOT VERIFIED |
| LEGACY-STAGE-6 | §4 L264 | 0–13 | Исторический gate: совместимость и аппаратные VM | PRESERVED / MAPPED |
| LEGACY-6-01 | §4 L265 | 10, 13 | Compatibility Runtime с подтверждённой матрицей приложений. | NOT VERIFIED |
| LEGACY-6-02 | §4 L266 | 10, 13 | Аппаратная виртуализация гостевых ОС, если аппаратная база и безопасность готовы. | NOT VERIFIED |
| LEGACY-STAGE-7 | §4 L268 | 0–13 | Исторический gate: выпуск | PRESERVED / MAPPED |
| LEGACY-7-01 | §4 L269 | 0–13 | Полный регрессионный прогон, security review, воспроизводимые артефакты, ISO и update archive. | NOT VERIFIED |
| LEGACY-7-02 | §4 L270 | 0–13 | Проверки на реальном оборудовании отдельно от эмуляции. | NOT VERIFIED |
| LEGACY-7-03 | §4 L271 | 0–13 | Документированная таблица известных ограничений. | NOT VERIFIED |
| LEGACY-EXIT-INTRO | §4 L273 | 0–13 | Подсистема считается готовой только при выполнении всех критериев: | GATE RULE |
| LEGACY-7-04 | §4 L274 | 0–13 | реализация присутствует в production-коде; | NOT VERIFIED |
| LEGACY-7-05 | §4 L275 | 0–13 | интерфейсы документированы; | NOT VERIFIED |
| LEGACY-7-06 | §4 L276 | 0–13 | есть позитивные и негативные тесты; | NOT VERIFIED |
| LEGACY-7-07 | §4 L277 | 0–13 | тесты фактически запущены на указанном commit SHA; | NOT VERIFIED |
| LEGACY-7-08 | §4 L278 | 0–13 | нет известных критических ошибок/заглушек; | NOT VERIFIED |
| LEGACY-7-09 | §4 L279 | 0–13 | межподсистемная интеграция проверена; | NOT VERIFIED |
| LEGACY-7-10 | §4 L280 | 0–13 | ограничения честно задокументированы. | NOT VERIFIED |
| STATUS-1 | §5 L285 | 0–13 | **Реализовано** — код существует и проходит нужные тесты. | POLICY |
| STATUS-2 | §5 L286 | 0–13 | **Частично** — часть функций работает; перечислить недостающие элементы. | POLICY |
| STATUS-3 | §5 L287 | 0–13 | **В разработке** — есть активная реализация, но критерии не закрыты. | POLICY |
| STATUS-4 | §5 L288 | 0–13 | **Не начато** — реализации нет. | POLICY |
| STATUS-5 | §5 L289 | 0–13 | **Не проверено** — код есть, но требуемый тест/железо не проверены. | POLICY |
| STATUS-6 | §5 L290 | 0–13 | **Заблокировано** — указать точную внешнюю/техническую причину. | POLICY |
| PLAN-RULE-1 | §6 L296 | 0–13 | Не удалять направления и требования молча. | POLICY |
| PLAN-RULE-2 | §6 L297 | 0–13 | Новые требования добавлять в соответствующий раздел и обновлять карту зависимостей. | POLICY |
| PLAN-RULE-3 | §6 L298 | 0–13 | Если требование признано невозможным или небезопасным, предложить альтернативу и записать решение с обоснованием; не вычёркивать самостоятельно. | POLICY |
| PLAN-RULE-4 | §6 L299 | 0–13 | При изменении архитектуры обновлять связанные API/ABI, тесты и документацию. | POLICY |
| PLAN-RULE-5 | §6 L300 | 0–13 | Любая новая задача должна ссылаться на один или несколько разделов этого плана. | POLICY |
| PLAN-RULE-6 | §6 L301 | 0–13 | Перед завершением рабочей сессии фиксировать точку продолжения в журнале проекта с commit SHA, изменёнными областями, фактическими тестами и следующими независимыми задачами. | POLICY |
| RELEASE-1 | §7 L306 | 13 | ISO для тестирования в Limbo x86 и QEMU; | NOT VERIFIED |
| RELEASE-2 | §7 L307 | 13 | отдельные образы/сборки для ARM64, когда они реально проверены; | NOT VERIFIED |
| RELEASE-3 | §7 L308 | 13 | архив обновления с исходниками/патчами и инструкцией установки; | NOT VERIFIED |
| RELEASE-4 | §7 L309 | 13 | SHA-256 всех артефактов; | NOT VERIFIED |
| RELEASE-5 | §7 L310 | 13 | точную команду Termux/Debian для сборки и копирования ISO в Android `Download`; | NOT VERIFIED |
| RELEASE-6 | §7 L311 | 13 | список проверенных функций и известных ограничений. | NOT VERIFIED |
| POLICY-8.1-01 | §8.1 L322 | 0–6, 11–13 | Целевое направление — гибридное capability-микроядро с постепенно выносимыми в user mode системными службами и драйверами. | ARCHITECTURE POLICY |
| POLICY-8.1-02 | §8.1 L323 | 0–6, 11–13 | Привилегированное ядро сохраняет минимальные механизмы: CPU/IRQ, адресные пространства, планирование, syscall entry, capability/object primitives и безопасное управление памятью. | ARCHITECTURE POLICY |
| POLICY-8.1-03 | §8.1 L324 | 0–6, 11–13 | HAL отделяет общие контракты от x86-64/AArch64 backends. | ARCHITECTURE POLICY |
| POLICY-8.1-04 | §8.1 L325 | 0–6, 11–13 | IPC должен быть типизированным, версионированным, ограниченным по размерам и правам; shared memory/ring buffers/zero-copy разрешены только с явными ownership/lifetime правилами. | ARCHITECTURE POLICY |
| POLICY-8.1-05 | §8.1 L326 | 0–6, 11–13 | Драйверы по возможности изолировать в user mode; MMIO/IRQ/DMA выдавать через capabilities и device-resource manager. DMA-capable драйверы не допускаются без fail-closed IOMMU policy. | ARCHITECTURE POLICY |
| POLICY-8.1-06 | §8.1 L327 | 0–6, 11–13 | KIRA Mind — отдельная ограниченная служба без неявных привилегий управления ядром. | ARCHITECTURE POLICY |
| POLICY-8.1-07 | §8.1 L328 | 0–6, 11–13 | KIRA Driver Manager не загружает неизвестные бинарные драйверы напрямую: provenance, license, source review, sandbox, ABI validation, build, signing and rollback обязательны. | ARCHITECTURE POLICY |
| POLICY-8.1-08 | §8.1 L329 | 0–6, 11–13 | Существующие рабочие пути сохраняются до доказанной parity и rollback; массовая перепись ради архитектурной чистоты запрещена. | ARCHITECTURE POLICY |
| POLICY-8.2-01 | §8.2 L333 | 9–10 | C + KCC — текущая основа ядра и критических низкоуровневых компонентов; сохранять и развивать, не переписывать автоматически. | ARCHITECTURE POLICY |
| POLICY-8.2-02 | §8.2 L334 | 9–10 | Assembly — только узкие boot/exception/context-switch/ISA-specific участки за контрактами HAL. | ARCHITECTURE POLICY |
| POLICY-8.2-03 | §8.2 L335 | 9–10 | Rust `no_std` — кандидат для новых изолированных служб и user-mode драйверов после подтверждения cross-target toolchain, panic/allocator strategy, ABI/FFI и воспроизводимых сборок x86-64/AArch64. | ARCHITECTURE POLICY |
| POLICY-8.2-04 | §8.2 L336 | 9–10 | WASM/WASI-профиль — переносимая среда приложений и расширений с capability-limited host API; существующий WASM runtime остаётся базой до сравнительного ADR. | ARCHITECTURE POLICY |
| POLICY-8.2-05 | §8.2 L337 | 9–10 | Python и Bash — host tooling, тестовая оркестрация и CI, не обязательные runtime зависимости ядра. | ARCHITECTURE POLICY |
| POLICY-8.2-06 | §8.2 L338 | 9–10 | SPARK Ada и Zig — только исследовательские кандидаты до отдельного ADR. | ARCHITECTURE POLICY |
| POLICY-8.2-07 | §8.2 L339 | 9–10 | Любой новый язык обязан иметь зафиксированные ABI, build integration, тесты, лицензионный/снабженческий аудит и критерии отказа/отката. | ARCHITECTURE POLICY |
| POLICY-8.3-01 | §8.3 L343 | 5, 7, 11 | FlatBuffers, Cap’n Proto и CBOR — конкурирующие кандидаты для typed IPC/config/metadata; выбрать по benchmark, fuzzing, schema evolution и security review, а не назначать один формат повсеместно. | ARCHITECTURE POLICY |
| POLICY-8.3-02 | §8.3 L344 | 5, 7, 11 | Arrow + Zstd + bounded event log — кандидат аналитического контура KIRA Mind; не переносить в критический syscall path без обоснования. | ARCHITECTURE POLICY |
| POLICY-8.3-03 | §8.3 L345 | 5, 7, 11 | Parquet — только долговременная аналитика/экспорт, если подтвердится практическая необходимость. | ARCHITECTURE POLICY |
| POLICY-8.3-04 | §8.3 L346 | 5, 7, 11 | CAS + Merkle DAG + CoW + signatures — целевое направление для целостности и атомарных объектов/пакетов, после crash-consistency и recovery tests. | ARCHITECTURE POLICY |
| POLICY-8.3-05 | §8.3 L347 | 5, 7, 11 | LSM-tree и B+Tree — выбирать по workload, write amplification, recovery и memory benchmarks. | ARCHITECTURE POLICY |
| POLICY-8.3-06 | §8.3 L348 | 5, 7, 11 | ELF и текущий WASM binary path сохранять до совместимого versioned executable/package contract. | ARCHITECTURE POLICY |
| POLICY-8.3-07 | §8.3 L349 | 5, 7, 11 | Все форматы должны иметь bounded parsing, versioning, canonical signature rules, fuzz tests и явную миграцию. | ARCHITECTURE POLICY |
| POLICY-8.4-01 | §8.4 L353 | 9 | Целевой pipeline: language frontends → MLIR where justified → KIRA IR/SSA → verifier/optimization → architecture backend → signed artifact. | ARCHITECTURE POLICY |
| POLICY-8.4-02 | §8.4 L354 | 9 | KIRA IR должен явно представлять фиксированные integer/float типы, bool, aggregate/enum/Option/Result, slices, ownership/lifetimes где применимо, atomics, endian types, capability handles и отдельные PhysAddr/VirtAddr/DmaAddr. | ARCHITECTURE POLICY |
| POLICY-8.4-03 | §8.4 L355 | 9 | Content-addressed cache и PGO внедрять после reproducible builds и базовых измерений. | ARCHITECTURE POLICY |
| POLICY-8.4-04 | §8.4 L356 | 9 | MLIR — компиляторная инфраструктура/bridge, не требование включать тяжёлую зависимость в ядро. | ARCHITECTURE POLICY |
| POLICY-8.6-01 | §8.6 L381 | 0–13 | конкретный workload и сравниваемые альтернативы; | ARCHITECTURE POLICY |
| POLICY-8.6-02 | §8.6 L382 | 0–13 | benchmark на x86-64 и AArch64, где это применимо; | ARCHITECTURE POLICY |
| POLICY-8.6-03 | §8.6 L383 | 0–13 | измерения latency, throughput, memory, code size, cold start, failure recovery и verifier cost; | ARCHITECTURE POLICY |
| POLICY-8.6-04 | §8.6 L384 | 0–13 | security review, bounded parser/fuzzing и lifecycle tests; | ARCHITECTURE POLICY |
| POLICY-8.6-05 | §8.6 L385 | 0–13 | license/provenance/supply-chain review; | ARCHITECTURE POLICY |
| POLICY-8.6-06 | §8.6 L386 | 0–13 | ADR с результатами, ограничениями, fallback и миграционным планом. | ARCHITECTURE POLICY |

## Ограничения текущего реестра

- Stage mapping — первичная маршрутизация, проверяемая в ходе source-level audit и dependency mapping.
- Реестр не доказывает, что требование реализовано, собрано, запущено в QEMU/Limbo или проверено на физическом оборудовании.
- Следующий шаг Stage 0: связать каждую строку A–N с точными source files, build targets, runtime dependencies, test cases и текущими findings; выявить orphan modules и uncovered requirements.
