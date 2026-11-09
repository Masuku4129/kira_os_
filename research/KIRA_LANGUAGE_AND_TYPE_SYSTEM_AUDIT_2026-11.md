# KIRA OS — аудит языков программирования, типов данных и ABI

**Дата:** 2026-11-09  
**База исходников:** `research/architecture-combination-2026-11`, наследует проверенный commit `13a9773a599bcebc0c9d282f43959268682bc9ce`  
**Цель:** закрыть пропуск в архитектурном исследовании: выбрать языки и модель данных для ядра, служб, драйверов, приложений, ИИ и KCC.

## 1. Краткий вывод

Нельзя назначить один язык для всех частей ОС. Лучшее практическое сочетание для KIRA на текущем этапе:

1. **C99/GNU C через KCC** — сохранить для существующего ядра, загрузочной логики и низкоуровневого кода, пока KCC не докажет совместимость с новым системным ABI. Это не утверждение, что C сам по себе безопасен.
2. **Rust `no_std`** — целевой язык для новых изолированных системных служб, драйверов user mode, парсеров, сетевых протоколов и менеджеров, где владение и проверка заимствований уменьшают ошибки памяти. Сначала сборка внешним `rustc` в CI для x86-64/AArch64; интеграция Rust в KCC не является условием и не должна ломать KCC.
3. **Assembler** — только неизбежные архитектурные участки: вход/выход прерываний, переключение контекста, ранний старт CPU и специальные инструкции. Остальную логику писать на C/Rust.
4. **WASM Component Model + WIT/WASI** — изолированный переносимый формат для приложений, плагинов и кода, созданного ИИ. Источником могут быть Rust/C и другие языки, если их toolchain выпускает совместимый компонент. WASM — формат исполнения, не язык ядра.
5. **Python** — инструменты сборки, генерация тестов, CI, анализ и обучение Mind; не системный runtime.
6. **Zig** — сравнительный кандидат для отдельных low-level модулей и cross-toolchain экспериментов, но не третий обязательный язык.
7. **Ada/SPARK** — кандидат для формально критических алгоритмов/контрактов, но не основной язык ОС на текущем этапе из-за отдельного toolchain и цены интеграции.

Не рекомендуется сейчас переписывать KIRA целиком на Rust, Zig или Ada. Не рекомендуется добавлять C++ как основной системный язык. Не нужно изобретать новый язык до того, как KIRA IR и ABI стабилизированы.

## 2. Что реально найдено в репозитории

Инвентаризация дерева на исследовательской ветке: 1093 tracked entries; по расширениям найдено 731 `.c`, 210 `.h`, 44 `.S`, 44 `.s`, 4 `.asm`, 27 `.py`, 29 `.sh`. Файлов `.rs`, `.zig`, `.cpp`, `.cc`, `.wasm`, `.wat`, `.ll` и `.mlir` в дереве не найдено. Это означает, что Rust/Zig/Ada и WASM Component Model пока не интегрированы как языковые/toolchain пути в этом checkout.

Основной системный код сейчас — C и assembler. Python используется в CI и инструментах.

### KCC сейчас

Документация `docs/KCC.md` определяет KCC как собственный C-компилятор с C99 и ограниченным GNU C:

- препроцессор, lexer, parser, AST, type checking, линейный IR;
- x86-64 и AArch64 генераторы;
- ELF output; свой x86 assembler/encoder;
- целые типы, указатели, массивы, struct/union/enum, typedef, bitfields, function pointers;
- qualifiers `const`, `volatile`, `restrict`; storage `static`, `extern`, `inline`;
- float/double, varargs, VLA, designated/compound initializers и выбранные GNU extensions;
- честные ошибки для неподдержанных конструкций.

Явно не поддержаны: `_Atomic` как ключевое слово C11, `_Complex`, `long double`, `_Thread_local`/TLS и computed goto. В `-mno-sse` сборке вещественные операции запрещены. Встроенные KCC atomic builtins существуют, но IR atomic сейчас описывает операции с порядком `seq_cst`; это не полноценная модель всех C11 memory orders.

KCC использует LP64: `char=1`, `short=2`, `int=4`, `long=8`, `long long=8`, указатель=8, `float=4`, `double=8`; enum представлен как 4-байтовый int. Указатель в `type.c` имеет размер 8, то есть текущий type model не является 32-битным универсальным ABI. Для x86-64 это соответствует LP64; для AArch64 также используется LP64.

KCC имеет отдельные ABI backend paths: System V AMD64 для x86-64 и AAPCS64 для AArch64. Это правильно — но KIRA должна зафиксировать собственный стабильный ABI для системных интерфейсов и явно отображать его на платформенные calling conventions.

### Что говорят тесты

В дереве KCC найдено:
- 139 положительных C99/GNU C тестов;
- 98 отрицательных тестов некорректных программ;
- 10 тестов неподдержанных конструкций;
- 4 теста намеренно более строгого поведения;
- 27 проверок константных выражений;
- 66 runtime/IR tests;
- 18 executable tests;
- 12 benchmarks;
- 12 ELF validation cases (x86-64 и AArch64).

Последний CI run `42547185559` содержит точное подтверждение: `tests/kcc/run.sh` сообщил **1373 файловых теста, 0 провалов**, затем 3000 fuzz итераций, 271 принятых и 2729 отклонённых входа. KCC test corpus завершился OK. Следующий шаг self-host в том же job провалился: `kcc_main.c:12: '../mind_build.h' file not found`. Поэтому корректно говорить: языковой корпус KCC и fuzz прошли; самосборка компилятора в этом прогоне — нет.

Эти тесты подтверждают значительный объём C frontend/type/runtime behavior, но **не подтверждают** безопасную типовую систему для всей ОС, Rust integration, ABI для внешних языков, pointer provenance, ownership, bounds safety или полную корректность на реальном ARM64 железе.

## 3. Сравнение языков для ролей KIRA

Оценки качественные и основаны на назначении языка и цене интеграции, а не на выдуманном бенчмарке KIRA.

| Язык/формат | Сильные стороны | Слабые стороны/цена | Роль в KIRA |
|---|---|---|---|
| C99/GNU C + KCC | Уже есть; прямой контроль памяти/ABI; малый runtime; подходит для boot/kernel | Не защищает от UAF, переполнений и неверных указателей; KCC не покрывает весь C | Сохранить для текущего ядра и низкоуровневого кода |
| Rust `no_std` | Ownership/borrowing, enums, slices, Result/Option, безопасная работа без std | Нужен rustc/LLVM toolchain, runtime/allocator/panic strategy и FFI audit; unsafe и DMA остаются | Основной кандидат для новых user-mode служб/драйверов |
| Zig | Явные аллокаторы, удобный cross-compilation, safety checks в safe mode | Ручное управление памятью; режимы сборки могут отключать safety checks; не даёт Rust-подобной модели владения | Сравнительный кандидат, не обязательный язык |
| Ada/SPARK | Сильная типизация, contracts, статические доказательства свойств | Новый toolchain, FFI и ecosystem cost; сложнее включить в существующий KCC pipeline | Формально критические алгоритмы как отдельный эксперимент |
| C++ | Большая экосистема, шаблоны и абстракции | Сложные object lifetime/ABI, исключения/RTTI/runtime; KCC не поддерживает | Не выбирать как системный язык сейчас |
| Python | Удобен для инструментов и автоматизации | Интерпретатор и динамический runtime не подходят ядру | CI, генераторы, анализ, инструменты Mind |
| WASM Component | Языконезависимая изоляция, ограниченные imports, стандартизованный ABI типов | Runtime и копирование/serialization cost; не прямой kernel/driver ABI | Приложения, плагины, AI-generated untrusted code |

### Почему Rust, но не «всё на Rust»

Rust `no_std` позволяет использовать `core` без системной библиотеки, что делает его пригодным для firmware/kernel/system services. Но для KIRA сегодня нет ни одного Rust source file, Rust target, rustc build step или C/Rust ABI test. Поэтому это рекомендация к поэтапному внедрению, а не утверждение, что KIRA уже поддерживает Rust.

Не переносить ядро в Rust одним шагом: сначала сделать отдельную маленькую user-mode службу, подключить её через C ABI и проверить, что сбой/перезапуск не рушит ядро. Затем проверить драйвер-процесс с ограниченными capabilities и DMA policy.

Zig достоин отдельного прототипа, если он покажет реальный выигрыш по времени сборки/кросс-компиляции или простоте low-level кода. Но он не заменяет Rust для ownership safety и не оправдывает третий обязательный toolchain.

SPARK особенно интересен для формально проверяемых частей: парсеров недоверенных структур, протокольных state machines, критичных политик разрешений. Но сначала сравнить затраты на доказательства и интеграцию с Rust + fuzzing + sanitizers.

## 4. Модель типов и данных: что оставить и что добавить

Нужно разделить три уровня, которые нельзя смешивать:

1. **Языковые типы** — что может выразить исходный код.
2. **Типы KIRA IR** — точная семантика операций для оптимизатора и codegen.
3. **ABI/wire types** — стабильный формат данных между ядром, процессами, драйверами, WASM и разными архитектурами.

### 4.1 Базовые типы

Зафиксировать в KIRA IR явные типы:
- `i8/i16/i32/i64/i128`, `u8/u16/u32/u64/u128`;
- `usize/isize` как target-width типы, запрещённые в persistent/wire formats;
- `f32/f64` IEEE-754; `f16` и vector types только при наличии реальной hardware/use-case проверки;
- `bool` с определённым ABI представлением;
- `char8`/byte отдельно от Unicode scalar `char`;
- `void`/unit;
- явные `ptr<T, address_space, mutability>` для raw pointers на низком уровне.

Для дисковых форматов, сетевых пакетов, системных сообщений и BootInfo использовать только fixed-width integers, явно заданный endian и version. Никогда не сериализовать `long`, `usize`, обычный указатель, enum без фиксированного representation или compiler-native struct layout.

### 4.2 Составные и безопасные типы

Добавить в целевую типовую модель:
- `struct/record` с явным layout и выравниванием;
- `enum` с фиксированным tag width для ABI;
- tagged `variant/union`, где tag проверяется до доступа к payload;
- `Option<T>` и `Result<T,E>` для отсутствующих значений и ошибок;
- `slice<T> = (pointer, length)`, чтобы длина не терялась при передаче буфера;
- immutable/mutable references и ownership/borrow metadata для Rust/AI-safe frontend;
- opaque handles вместо raw pointers для IPC, объектов ядра, устройств и DMA;
- `NonNull<T>`/nullable pointer distinction;
- bounded integer/range types для длины, индексов, размеров и квот;
- endian-specific `Le16/Le32/Le64`, `Be16/Be32/Be64`;
- отдельные semantic newtypes: `PhysAddr`, `VirtAddr`, `DmaAddr`, `FileOffset`, `TimeNs`, `CpuId`, `ProcessId`, `CapabilityId`, `DeviceId`.

Нельзя считать два одинаковых по ширине целых взаимозаменяемыми, если смысл различается: физический адрес нельзя случайно использовать как виртуальный, DMA address нельзя считать обычным physical address, а capability ID нельзя разыменовывать как указатель.

### 4.3 Указатели, владение и время жизни

Текущий C/KCC type model знает тип указателя и qualifiers, но не доказывает:
- что указатель ненулевой;
- что диапазон памяти целиком доступен;
- что объект ещё жив;
- что нет data race/неправильного aliasing;
- что DMA устройство больше не обращается к буферу;
- что владелец имеет право читать/писать объект.

Поэтому целевая система должна различать:
- raw pointer — только в низкоуровневом unsafe-коде;
- reference — проверенное существование и lifetime;
- slice — pointer + length;
- owned buffer — единственный владелец;
- shared immutable buffer;
- synchronized mutable buffer;
- DMA buffer — отдельное состояние владения CPU/device и явный sync/unmap;
- capability handle — право на операцию, не адрес памяти.

Для C драйверов эти правила обеспечиваются wrapper API, проверкой длины, ownership protocol и тестами. Для Rust — типами и borrow checker там, где не пересекается unsafe/FFI.

### 4.4 Атомарность и конкурентность

Текущий KCC IR имеет атомарные операции с seq_cst. Этого недостаточно для оптимального ядра: нужны memory order `relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`, fences и корректная модель для x86-64 и AArch64.

Ввести типы и правила:
- `Atomic<T>` только для разрешённых integer/pointer типов и допустимого выравнивания;
- explicit memory order;
- `MutexGuard<T>`, `SpinLockGuard<T>`, `RwLockGuard<T>`;
- типизированные message queues и waitable handles;
- compile-time запрет на обычную запись в shared mutable object без синхронизации на safe frontend.

Нужны litmus tests для store-buffering, message-passing, CAS loops, ABA, torn reads, acquire/release и seq_cst на обеих архитектурах. Сравнить результаты с эталонной моделью и аппаратным QEMU; отдельно не путать QEMU с реальным weak-memory hardware validation.

### 4.5 Структуры и layout

В KIRA нужен один документированный ABI:
- C `repr(C)`-совместимые структуры только на границах, где это необходимо;
- явные size/alignment/offset assertions;
- fixed tag/discriminant width;
- packed layouts только для hardware/network/disk wire structures, никогда для обычных shared-memory объектов без unaligned-safe accessors;
- padding bytes не должны содержать секреты и не должны сравниваться как семантические данные;
- versioned messages и reserved fields;
- separate x86-64 System V / AArch64 AAPCS64 calling convention adapters;
- no pointers in persistent storage or cross-process messages unless represented as capability/handle.

WASM Component Model/WIT должен быть внешним высокоуровневым ABI: bool, fixed-width ints, float, char, string, list, record, variant, option, result, resource handles. Не экспортировать сырые адреса KIRA и native struct layout в WASM.

## 5. Рекомендуемая языковая архитектура

| Слой KIRA | Язык/формат | Правило |
|---|---|---|
| Ранний boot, exception entry, context switch | Assembler + C | Только минимальные CPU-specific фрагменты |
| Текущее ядро | C99/GNU C, KCC | Не ломать работающий путь; строгий warning/sanitizer режим на host |
| Новые user-mode службы | Rust `no_std` | Начать с простого процесса без DMA |
| Драйверы user mode | Rust `no_std` предпочтительно, C/KCC допустим | Capability-limited, IOMMU gated |
| Существующие/совместимые драйверы | C/KCC | Только разрешённый исходный код и стабильный KDRV ABI |
| AI-generated native programs | Rust/C frontend → KIRA IR → KCC или verified backend | Код ИИ всегда untrusted до проверки |
| AI-generated extensions/apps | WASM Component + WIT | По умолчанию без ambient permissions |
| Сборка/CI/анализ | Python + shell | Не часть runtime |
| Формально критичные алгоритмы | SPARK prototype | Только после доказанного выигрыша |
| Сравнительный low-level toolchain | Zig prototype | Не обязательная зависимость |

Ключевой момент: **KCC остаётся собственным компилятором, но не обязан сразу стать компилятором всех языков.** KCC должен владеть KIRA IR, native ABI и генерацией машинного кода. Другие фронтенды (начиная с Rust через официальный compiler toolchain) могут выдавать проверяемый объект/IR с чётким контрактом. В долгосрочной перспективе KCC может принять KIRA IR как стабильный вход, а не парсить Rust source.

## 6. Требования к KIRA IR

Текущий IR — линейный набор инструкций с vreg, width/sign, calls, memory ops, floats, atomics и backend-specific details. Для нового архитектурного ядра он должен стать typed CFG/SSA и явно хранить:

- integer bit width + signedness;
- float format;
- pointer address space, nullability, provenance, alignment;
- struct/array layout, field offsets, packed/unaligned flag;
- ownership, mutability, lifetime region, borrow kind;
- alias/effect information;
- atomic ordering and synchronization scope;
- capability requirements/effects;
- endian for serialized values;
- calling convention and ABI aggregate classification;
- trap/overflow semantics (wrap, checked, saturating, poison/UB policy);
- source-language identity and debug location;
- target features and address-space constraints.

Не делать unsafe/ownership metadata декоративными флагами: verifier должен проверять их до codegen, а оптимизатор обязан сохранять семантику. Каждый IR pass должен иметь invariant tests и differential tests.

## 7. Тестовая матрица языков и типов

### Уже существующее покрытие

- C99/GNU C syntax/type acceptance/rejection против clang;
- целочисленные promotions, signed/unsigned arithmetic, literals, shifts, sizeof, alignas, struct/union layout, bitfields;
- pointers, arrays, function pointers, VLAs, aggregates by value, varargs;
- float/double arithmetic, casts, NaN, comparisons and softfp tests;
- x86-64 and AArch64 ELF cases;
- 1373 KCC file tests + 3000 fuzz iterations passed in run 42547185559;
- KCC self-host in that same run failed at a missing include path, after the corpus itself had passed.

### Что обязательно добавить до выбора окончательного языка/IR

1. **Integer semantics:** every width/sign pair; overflow/wrap/checked/saturating; shifts at -1, 0, width-1, width, width+1; division min/-1; promotions; constant folding parity.
2. **Pointer safety:** null, one-past, alignment, provenance, integer-pointer round trips, invalid address spaces, pointer arithmetic overflow, bounds checks, use-after-free tests in safe frontend.
3. **Aggregate layout:** size/align/offset parity, nested struct, unions, tagged variants, zero-sized fields/types, packed/unaligned access, bitfield endian/ABI tests.
4. **Floating point:** IEEE binary32/64, NaN payload policy, signed zero, infinities, subnormals, rounding, fused operations, denorm modes, no-FPU kernel build.
5. **Atomics:** all memory orders, lock-free widths, alignment, CAS expected-value semantics, AArch64 litmus tests, compiler reorder tests.
6. **Ownership/lifetimes:** double free, use-after-free, dangling reference, mutable alias, partial initialization, destructor/panic/unwind policy, FFI lifetime escape.
7. **IPC/WASM types:** strings, lists, nested records, variants, option/result, resource handles, async stream/future, invalid length, huge allocation, serialization fuzz.
8. **ABI:** C↔Rust calls, structs by value and return, varargs policy, callbacks, unwind forbidden across C ABI, x86-64 SysV and AArch64 AAPCS64.
9. **Cross-target:** same test vectors and serialized bytes on x86-64 and AArch64; explicit endian tests; no assumption that target-sized integers are stable on disk/wire.
10. **Compiler correctness:** KCC vs clang differential; Rust frontend vs reference rustc; IR verifier mutation tests; optimization on/off equivalence; reproducible builds; bootstrap stage equality.
11. **Security:** malicious source comments/docs prompt injection, malicious IR, invalid type metadata, compiler resource exhaustion, unsafe operation gating, capability forgery attempts.
12. **Performance:** compile time, incremental rebuild, binary size, runtime, memory footprint, IPC conversion/copy overhead, bounds/ownership checks, WASM boundary cost.

### Acceptance gates

- No ABI struct without compile-time size/alignment/offset assertions.
- No persistent/wire format contains native pointers, `long` or `usize`.
- All cross-language FFI passes a versioned ABI conformance suite on both architectures.
- No raw pointer crosses process/WASM boundary; only opaque capabilities/handles and bounded buffers.
- All integer semantics tests agree between KCC and reference compiler for supported language subset.
- Atomic tests pass on x86-64 and AArch64; no claim of real hardware weak-memory proof from QEMU alone.
- Rust services compile `no_std`, link with the KIRA ABI, and survive process crash/restart tests before they become production dependencies.
- Language and type safety is not accepted solely because a compiler exits successfully.

## 8. Implementation order

1. Freeze KIRA ABI v1: fixed-width primitives, struct layout rules, capability handles, errors, versioning, endian.
2. Add type/ABI conformance tests to KCC and CI. Separate language tests from benchmark/self-host gates so one failure cannot hide the 1373 corpus result.
3. Fix KCC selfhost include sandbox path; re-run corpus, fuzz, selfhost, x86-64 and AArch64 tests.
4. Add explicit memory-order model to KCC IR/backend and test on x86-64/AArch64.
5. Prototype one tiny Rust `no_std` user service for each architecture, called through KIRA ABI; do not yet migrate a critical driver.
6. Prototype WIT interface for one application/driver-manager service and fuzz canonical ABI conversion.
7. Compare a small Zig module and SPARK proof exercise only if they answer a concrete gap.
8. Only after evidence decide whether any additional frontend belongs in KCC/KIRA IR.

## 9. Final decision after language/type audit

**The previous architecture recommendation is incomplete and must be amended.** The revised candidate stack is:

- C99/GNU C + KCC for current kernel and low-level compatibility;
- minimal assembly for architecture entry paths;
- Rust `no_std` for new isolated system services and user-mode drivers after ABI/toolchain tests;
- typed KIRA IR as the common compiler/verifier contract;
- fixed-width, endian-explicit KIRA ABI with capability handles and safe aggregate types;
- WASM Component Model/WIT for untrusted portable applications and AI-generated extensions;
- Python only for tooling/CI;
- Zig and SPARK remain optional comparative experiments, not mandatory system languages.

**What has been tested:** existing C/KCC language corpus and fuzzing, as recorded in CI.  
**What has not been tested:** Rust/Zig/SPARK toolchains in KIRA, C↔Rust ABI, full typed-IR invariants, ownership enforcement in KIRA, all atomic memory orders, cross-language WIT integration, and physical weak-memory hardware.

Therefore this is a stronger, evidence-backed *candidate language and type architecture*, not yet a fully validated final stack.

## 10. Technical references

- Rust `no_std` for firmware/kernel use: https://doc.rust-lang.org/embedded-book/intro/no-std.html
- Linux kernel Rust support and `no_std`: https://docs.kernel.org/rust/general-information.html
- Zig language reference (safety modes and explicit allocator model): https://ziglang.org/documentation/
- SPARK Ada contracts, memory safety and proof: https://www.adacore.com/languages/spark
- WebAssembly Component Model canonical ABI: https://component-model.bytecodealliance.org/advanced/canonical-abi.html
- WIT/component model values and resources: https://github.com/WebAssembly/component-model/blob/main/design/mvp/Explainer.md
- C fixed-width integer types: https://en.cppreference.com/w/c/types/integer
