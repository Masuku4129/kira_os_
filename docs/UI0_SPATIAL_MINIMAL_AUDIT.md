# KIRA OS — UI-0 инвентаризация текущей графической оболочки

**Основание:** разделы J/J.1 мастер-плана и `docs/KIRA_DESIGN_SYSTEM.md`.  
**Базовый commit:** `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4` (`main`, 2026-11-12).  
**Статус:** статическая инвентаризация исходников; runtime-проверка и визуальное сравнение на этом этапе не выполнялись.

## 1. Что уже существует

| Область | Фактическая реализация | Вывод для Spatial Minimal |
|---|---|---|
| Композитор и окна | `wm.c/.h`: окна, z-order, фокус, перемещение, масштабирование, fullscreen, damage tracking, параллельная композиция | Сохранить как основу; не переписывать оконную модель ради нового desktop |
| Верхняя панель | `WM_TOP_H=40`; бренд, вкладки-разделы, дата/время, загрузка ядер, память и здоровье Mind | Сейчас постоянно занята навигацией и телеметрией; должна стать компактной системной зоной либо контекстным слоем |
| Левая панель | `WM_SIDE_W=76`; закреплённые и запущенные приложения, hover-подсказки, запуск/возврат окон | Это одновременно launcher и список окон, то есть традиционная taskbar-модель; заменить отдельной минимальной навигацией, не ломая запуск приложений |
| Dock model | `dock.c/.h`; пользовательский список в `/var/dock`, до 10 программ, parse/format/add/remove | Модель данных переиспользуема; текущая привязка к левой панели и названия/комментарии требуют отделения от визуального размещения |
| Оконная рамка | Заголовок и кнопки закрыть/свернуть/fullscreen, focus glow | Сохранить оконные действия; позже проверить более тихую рамку и поведение без лишнего свечения |
| Фон | `brainfx.c/.h` строит декоративную анимацию мозга/сети; фон создаётся в `wm_init` | Декоративная анимация не должна быть обязательной; нужны спокойный фон, выключение эффектов и независимость читаемости от обоев |
| UI primitives | `gfxui.c/.h`: alpha blend, rounded rect, rings/arcs, сглаженный текст, градиенты, glow, cards/buttons | Достаточная база для токенов и поверхностей; проверить стоимость glow/alpha на слабых устройствах |
| Theme | `theme.h`: тёмная сине-циановая палитра, отдельный фиолетовый акцент, размеры карточек | Нужна семантическая палитра и контрастные состояния; не сводить весь интерфейс к постоянному неоновому свечению |
| Тесты | `tests/wm_test.c`, `tests/dock_test.c`, `tests/ui_test.c`, `tests/gfx_test.c` | Есть тесты поведения окон, Dock-модели и примитивов; нет отдельного набора acceptance-тестов Spatial Minimal |

## 2. Подтверждённые архитектурные конфликты с новым дизайном

1. `wm_test.c` прямо ожидает верхнюю панель и боковую панель, проверяет их геометрию и клики по ним. Изменение layout потребует перевести эти проверки на контракт режима/поведения, а не просто удалить assertions.
2. `wm_set_clock()` и `wm_set_meters()` помечают верхнюю полосу на перерисовку. Метрики CPU/RAM/Mind сейчас являются частью постоянного chrome, а не контекстной системной поверхности.
3. `draw_side()` визуально смешивает закреплённые приложения и работающие окна. Это противоречит требованию не воспроизводить традиционную панель задач.
4. `dock_t` уже отделяет список закреплённых программ от рендера, но комментарии, API-алиасы и интеграция называют его левой панелью. Нужно отделить модель закрепления от геометрии Dock.
5. UI-примитивы поддерживают прозрачность и свечение, но дизайн-контракт требует, чтобы читаемость и контраст не зависели от прозрачности/обоев. Нужны проверяемые opaque fallback и контрастные состояния.

## 3. Что нельзя ломать при переходе

- Создание/закрытие/сворачивание/возврат окон и корректный focus/z-order.
- Fullscreen и обработку ввода приложений Ring 3.
- Существующие API приложений и запуск из `/bin`.
- Damage tracking, partial redraw и `wm_compose_par()`.
- Пользовательские закрепления `/var/dock`.
- Работу на 1024×768 и других проверяемых разрешениях; поддержку кириллицы.
- Host/QEMU/Limbo сценарии. Эмуляторные проверки не выдавать за тесты физического оборудования.

## 4. Безопасная последовательность UI-работ

### UI-0 — инвентаризация (этот документ)
- Зафиксированы существующие модули, UI-контракты, тесты и конфликты.
- Не менялись production-файлы и не заявлялась runtime-готовность.

### UI-1 — токены и layout contract
- Ввести семантические токены цвета, типографики, размеров, радиусов, отступов, состояний и motion policy.
- Определить отдельные поверхности: persistent shell, Dock, app switcher/search, system quick panel, notifications, KIRA AI.
- Согласовать keyboard/touch/mouse entry points и поведение при узкой ширине.
- Не вводить новый compositor и не дублировать оконный менеджер.

### UI-2 — минимальная оболочка за feature gate
- Добавить новый layout mode, не меняя существующие window/app APIs.
- Чистое центральное пространство; компактная навигация; небольшой Dock; одна сдержанная system zone.
- Не показывать CPU/RAM-графики, список окон, историю уведомлений или большой AI widget постоянно.
- Сохранить legacy layout как временный fallback до прохождения регрессий.

### UI-3 — контекстные поверхности
- Открытие launcher/search, KIRA AI, системной диагностики и уведомлений только по действию/событию.
- Критические уведомления не крадут фокус; группировка и отложенный просмотр.

### UI-4 — адаптация и доступность
- Проверить 4:3, 16:9, 21:9, DPI, масштабирование, клавиатуру, мышь и touch.
- Reduced motion, видимый focus, читаемость на светлом/тёмном фоне и без прозрачности.

### UI-5 — регрессии и измерения
- Обновить `wm_test` на контракт обоих layout modes; добавить тесты инвариантов чистого desktop.
- Проверить идентичность полной и damage-перерисовки.
- Сравнить CPU time/frame, число перерисованных пикселей и память на одинаковых сценариях.
- Прогнать host suite, GUI regression и QEMU/Limbo; указывать точный commit SHA и окружение.

## 5. Минимальные критерии Spatial Minimal

- В idle-состоянии центральная область свободна; нет постоянного launcher/task list/telemetry dashboard.
- Dock показывает только закреплённые или недавно выбранные приложения в ограниченном количестве; не превращается в список всех окон.
- KIRA AI доступен одним понятным действием, но его большая панель закрыта по умолчанию.
- CPU/RAM/disk/Mind, журнал, диагностика и история уведомлений открываются контекстно.
- Ни одна ключевая функция не зависит от hover, анимации, прозрачности или обоев.
- Старые сценарии окон и приложений продолжают работать.

## 6. Следующая конкретная задача

До изменения `wm.c` проследить место формирования `wm_side_set()`, `wm_tabs_set()`, `wm_set_meters()` и жизненный цикл `dock_t` в системном цикле; затем подготовить UI-1 token/layout contract и отдельные тестовые acceptance cases. Не выполнять массовую замену верхней/левой панелей до этого анализа.


## 7. Wiring дисплейной службы (проверено после первичного аудита)

- `display_init()` создаёт WM и терминал, загружает `dock_default()`, вызывает `side_rebuild()` и регистрирует `wm_tabs_set()`.
- `side_rebuild()` собирает в одном массиве закреплённые приложения, запущенные незакреплённые приложения и команду «Добавить программу»; затем передаёт всё в `wm_side_set()`. Поэтому список окон и Dock действительно связаны не только визуально, но и в display orchestration.
- `dock_toggle()` изменяет `dock_t`, сохраняет `/var/dock` через `explorer_write()`, пересобирает боковую панель. Модель закрепления можно оставить, заменив только projection/rendering.
- `update_mind()` передаёт CPU/memory/health в `wm_set_meters()`; `update_clock()` обновляет верхнюю панель. Оба канала должны быть отвязаны от idle desktop в новом layout.
- `on_tab()` открывает системные приложения либо сворачивает все окна для «Главной». Эту семантику следует сохранить за новым навигационным API, а не за вкладками top bar.
- `wm_test.c` проверяет боковую панель, вкладки, часы, meters, hit-testing, запуск и возврат окон. Это главный GUI regression gate при переключении layout.
- В `display_init()` startup log прямо описывает legacy top bar/side launcher/neural background; его также нужно обновить только после фактической реализации нового режима.


## 8. UI-2 implementation progress (2026-11-13)

- Feature gate is present in `wm_layout_mode_t`; legacy remains the `wm_init()` default.
- Spatial Minimal currently renders a compact system zone, icon-only navigation rail, pinned-only bottom Dock, and a quiet canvas. Existing tab actions remain the destination handlers.
- Navigation labels are shown only for the hovered destination; the rail itself uses the existing authored icon set. Tooltip damage is included in the partial-redraw region.
- The running-app list and the “add application” action are intentionally not projected into the persistent Dock.
- `tests/wm_test.c` covers legacy default, mode switching, navigation hit-testing/callbacks, pinned-only Dock filtering, restore behavior, hidden telemetry, and full-vs-damage redraw equivalence.
- This is still a first UI-2 vertical slice, not the full Spatial Minimal experience. App discovery/search, temporary window overview, KIRA AI entry, quick system/diagnostics/notifications, workspace switching, responsive overflow, DPI/touch/accessibility and performance baselines remain open.
- GitHub Actions run `43380318838` was pending on the preceding code head `f992cd55c0bd3f4ca464745ee8f4a25e5d391a95` when this note was prepared. The documentation update below changes the branch head; only a run on the final updated SHA can verify the final PR head.
- No local build or physical hardware test was performed in this session.


## 9. UI-2 — временный обзор окон (реализация в работе)

- В wm.h добавлен отдельный контракт временного обзора окон; существующие API создания/уничтожения/фокуса/видимости не изменены.
- В wm.c обзор строит актуальный список по z-order, включает свёрнутые окна, исключает закреплённые системные виджеты, показывает миниатюры содержимого и состояние окна.
- В Spatial Minimal обзор открывается кнопкой «ОКНА» в верхней системной зоне; выбор карточки восстанавливает окно и поднимает его наверх. Есть закрытие и постраничная навигация для большого числа окон.
- Список сверяется с жизненным циклом окон перед композицией; выбранный ID и generation дополнительно проверяются перед восстановлением, чтобы не принять повторно использованный slot за старое окно.
- Изменение содержимого, заголовка или видимости обновляет соответствующую карточку; обновление скрытого приложения не требует показа его окна.
- tests/wm_test.c дополнен сценариями включения, подсчёта, отображения, full-vs-damage redraw, восстановления свёрнутого окна и закрытия overlay.
- Дополнительно изолирован pointer input: пока overlay открыт, события не передаются нижележащим приложениям; release после выбора поглощается. Добавлен WM_INPUT regression test.
- Проверка сборкой/host-tests на новом head ещё ожидается. Визуальная проверка в Limbo и на физическом устройстве не выполнена.


## 10. UI-3 — app discovery/search: confirmed integration constraints

- Каталог встроенных Ring 3 программ формируется сборкой: tools/kfstool manifest генерирует PROG_MANIFEST из ELF-файлов, которые сборка кладёт в /bin. Это предпочтительнее сканирования /bin из потока дисплея.
- progs.c::prog_list() синхронно читает и хеширует каждый бинарный файл; GUI/display thread его не вызывает.
- Контракт очереди уже реализован в prog_queue.c: состояния IDLE/PENDING/RUNNING/COMPLETE, атомарное принятие одного запроса, явные ACCEPTED/BUSY/INVALID, request ID и однократное получение результата. Слот остаётся занятым до получения результата; второй запрос не затирает первый.
- Имя и путь проверяются до копирования; слишком длинный путь отклоняется, а не обрезается. Host-тесты покрывают границы, BUSY на этапах PENDING/RUNNING/COMPLETE, completion и конкурентную отправку 12 потоками.
- `prog_request()`/`prog_request_file()` — безопасный интерфейс для GUI; `prog_poll()` исполняет запрос в основном цикле ядра. Окно Launcher ещё не подключено к этому контракту.
- Проверка доверия должна оставаться в prog_run_ex(): имя и SHA-256 сверяются с встроенным манифестом непосредственно при запуске. Launcher не должен обходить эту проверку или исполнять найденный ELF напрямую.
- Встроенный манифест включает не только обычные приложения, но и тестовые/атакующие/benchmark binaries (например atk-*). Каталог должен иметь явную мета-классификацию и не показывать security-test binaries как обычные пользовательские приложения.
- display_main() сейчас забирает клавиатуру только когда Проводник сверху; display_kbd_owner() сообщает консоли только files_focus. Launcher должен получить эксклюзивный keyboard focus и подавлять console input, иначе ввод поиска уйдёт в командную строку.
- apps.c/h — слой рисования и hit-testing; display.c — orchestration/state; WM input callback — mouse events. Новый launcher entrypoint следует добавить только в Spatial Minimal (отдельный rail affordance/контекстный вызов), не добавляя legacy top-bar tab.
- Следующий вертикальный срез: curated catalog model + async launch acceptance/result; затем search field/keyboard ownership; затем UI cards/categories/empty/error states и тесты trust gating, queue busy, search, keyboard focus и launch feedback.


## 11. UI-3 — curated build-time application catalog

- kfstool catalog generates an immutable catalog header from each architecture's actual ELF input list. Only names in an explicit end-user allowlist are emitted; absent binaries are not listed.
- Security tests, attack samples, benchmarks and internal helpers are excluded by default. The trust manifest remains independent and continues to include every executable required for launch verification.
- Metadata contains program ID, Russian title, category and short description. The display path never scans /bin, reads ELF files or hashes binaries.
- prog_catalog_count/get/search expose the generated catalog. Search is substring-based and case-insensitive for ASCII and Russian Cyrillic, including Ё/ё.
- x86-64 and ARM64 generate their own catalog header. It is not a trust source; prog_run_ex() continues SHA-256 verification against PROG_MANIFEST.
- Host coverage verifies allowlist filtering, generated membership, invalid indices, empty query, result capacity and mixed-case Russian/English search. Truncated UTF-8 search input is also covered to prevent reads past the string terminator.
- The catalog service is compiled for x86-64 and ARM64 from the architecture-specific generated `app_catalog.h`; the ARM64 source list now includes `prog_catalog.c`.
- This slice supplies the catalog model only. Launcher cards, live search input, keyboard ownership and launch selection UI remain open.
- Scope: master plan J/J.1, K, F and N; workstreams 2, 5, 9 and 12.
