# RFC-SVC-001: граница между syscall ABI и системными службами

**Статус:** Draft — переходное разделение кода; не утверждает, что службы уже работают отдельными процессами.  
**Контракты:** ABI-001, SVC-001, HOST-001, CAP-001  
**Базовая реализация:** ветка `architecture/target-baseline-2026-11`

## 1. Цель

Отделить стабильный вход системных вызовов и механизмы ядра от знания о конкретных подсистемах ОС. Пользовательский Native ABI остаётся стабильным, а реализация файловых, графических, сетевых и аудиофункций получает отдельную точку адаптации.

Это первый структурный шаг к Service Manager и отдельным системным службам. Он **не** создаёт IPC-процессы и не повышает отказоустойчивость сам по себе.

## 2. Существующее поведение, которое сохраняется

- Архитектурный вход SYSCALL/SVC остаётся в `arch/x86_64` и `arch/aarch64`.
- Общий syscall handler продолжает считать вызовы, проверять capability для операций, выполнять процессный post-return check и обслуживать kernel primitives.
- Номера syscall — часть Native ABI. Их значения нельзя менять без отдельного ABI RFC, версии и миграционного плана.
- Валидация указателей, копирование user memory, ограничения размеров и проверка полномочий остаются обязательными.
- Существующие коды возврата, timeout, ownership дескрипторов и поведение блокирующих вызовов сохраняются.

## 3. Разделение ответственности

### Kernel syscall core — `kernel/syscall.c`

Оставляет:
- syscall entry-facing handler и общий post-dispatch lifecycle;
- процесс/поток: exit, pid/tid, create thread;
- scheduling primitives: sleep/yield/ticks;
- память процесса: heap/JIT;
- futex, port, shared-memory и handle primitives;
- monotonic clock;
- capability gate и driver-specific privileged syscalls;
- общую безопасную работу с user pointers, где она нужна kernel mechanisms.

Ядро отвечает за безопасный переход через privilege boundary и примитивы, а не за реализацию политики файлового менеджера, TCP или аудиомикшера.

### Service-facing syscall adapters

`services/syscall_router.c` — subsystem-agnostic router. Он не включает заголовки графики, файлов, сети, звука или RTC и только передаёт запрос специализированному adapter:
- `services/graphics_syscall.c`: `SYS_GFX_*` → gfx service API;
- `services/filesystem_syscall.c`: `SYS_FS` → file-service API;
- `services/network_syscall.c`: TCP/UDP/DNS syscalls → network API;
- `services/audio_syscall.c`: playback/record syscalls → audio API;
- `services/clock_syscall.c`: `SYS_TIME` → RTC API.

Каждый ABI номер имеет ровно одного владельца-адаптер, что закреплено `tests/architecture_test.py`. Adapter — внутренний kernel facade. Это ещё не отдельный процесс службы, не IPC и не гарантия crash isolation.

### ABI number registry — `kcc/include/syscall_numbers.h`

Содержит канонический набор числовых идентификаторов. Kernel-side `syscall_numbers.h` — forwarding header; `userland/kira.h` и `kcc/include/kira.h` подключают тот же канонический файл. Таким образом kernel, встроенный KCC и приложения используют одну таблицу номеров. Значения перенесены без изменения.

## 4. Целевая эволюция

1. **Текущий шаг:** разнести core dispatcher и service adapters по отдельным translation units, сохранив поведение и номера.
2. Зафиксировать типизированные request/response структуры и версию каждого service API; прекратить передачу сырых ABI регистров глубже adapter.
3. Ввести capability-bound service endpoints и IPC transport для выбранной службы.
4. Перенести службу в user mode только после готовности Service Manager, readiness, restart, peer-death, backpressure и crash-recovery contracts.
5. Оставить syscall ABI как стабильный фасад совместимости; его внутренний маршрут может перейти с прямого вызова на IPC без изменения приложения.

Переход выполняется по одной службе и одному контракту за раз. Нельзя одномоментно перевести все syscalls на IPC до готовности транспорта и управления жизненным циклом.

## 5. Security requirements

- Capability enforcement не переносится в adapter и не ослабляется.
- Служба не может самостоятельно выдать capability вызывающему процессу.
- Identity вызывающего процесса должна быть получена из доверенного kernel context, не из user-supplied payload.
- При переходе на IPC endpoint должен быть capability-bound; peer identity и право на каждую операцию проверяются на стороне службы.
- Ошибки adapter/IPC должны быть определёнными и не превращаться в успешные нулевые ответы.
- Service crash не должен приводить к use-after-free handles, зависанию навсегда или kernel panic.

## 6. Build and verification

Оба target build обязаны включать `services/syscall_router.c`:
- x86-64: `build_clang.sh`;
- ARM64: `build_arm64.sh`.

Acceptance для текущего extraction:
- номера syscall до/после совпадают побайтно;
- каждый service syscall из исходного dispatcher обрабатывается ровно одним adapter case;
- core не включает `net.h`, `audio.h`, `gfxsrv.h`, `fsrv.h` или `rtc.h`;
- x86-64 и ARM64 сборки проходят с `-Werror`;
- host tests и QEMU scenarios проходят;
- приложения и ABI smoke tests демонстрируют прежние коды возврата и поведение.

Acceptance для будущего отдельного service process дополнительно включает malformed IPC, peer death, service restart, permission denial, resource exhaustion, fuzzing и end-to-end app scenarios.

## 7. Non-goals

- Не менять номера syscalls и userland calling convention.
- Не переписывать сеть/графику/звук/FS.
- Не утверждать, что существующие службы уже изолированы от ядра.
- Не добавлять фиктивный Service Manager или пустые реализации.
- Не менять capability model в рамках этого RFC.
