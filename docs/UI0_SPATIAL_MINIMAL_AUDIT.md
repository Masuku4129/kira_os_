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
- `prog_request()`/`prog_request_file()` — безопасный интерфейс для GUI; `prog_poll()` исполняет запрос в основном цикле ядра. Новый Launcher подключает `prog_request()` и получает итог через `prog_request_take_result()`.
- Проверка доверия должна оставаться в prog_run_ex(): имя и SHA-256 сверяются с встроенным манифестом непосредственно при запуске. Launcher не должен обходить эту проверку или исполнять найденный ELF напрямую.
- Встроенный манифест включает не только обычные приложения, но и тестовые/атакующие/benchmark binaries (например atk-*). Каталог должен иметь явную мета-классификацию и не показывать security-test binaries как обычные пользовательские приложения.
- `display_main()` маршрутизирует клавиатуру одному владельцу: Launcher сверху получает ввод первым, иначе его получает Проводник. `display_kbd_owner()` сообщает консоли о любом из этих владельцев.
- apps.c/h — слой рисования и hit-testing; display.c — orchestration/state; WM input callback — mouse events. Новый launcher entrypoint следует добавить только в Spatial Minimal (отдельный rail affordance/контекстный вызов), не добавляя legacy top-bar tab.
- Следующий срез: категории и фильтры, явные состояния загрузки/ошибки, управление фокусом мышью/сенсором, проверка короткого экрана и регрессии клавиатуры.


## 11. UI-3 — curated build-time application catalog

- kfstool catalog generates an immutable catalog header from each architecture's actual ELF input list. Only names in an explicit end-user allowlist are emitted; absent binaries are not listed.
- Security tests, attack samples, benchmarks and internal helpers are excluded by default. The trust manifest remains independent and continues to include every executable required for launch verification.
- Metadata contains program ID, Russian title, category and short description. The display path never scans /bin, reads ELF files or hashes binaries.
- prog_catalog_count/get/search expose the generated catalog. Search is substring-based and case-insensitive for ASCII and Russian Cyrillic, including Ё/ё.
- x86-64 and ARM64 generate their own catalog header. It is not a trust source; prog_run_ex() continues SHA-256 verification against PROG_MANIFEST.
- Host coverage verifies allowlist filtering, generated membership, invalid indices, empty query, result capacity and mixed-case Russian/English search. Truncated UTF-8 search input is also covered to prevent reads past the string terminator.
- The catalog service is compiled for x86-64 and ARM64 from the architecture-specific generated `app_catalog.h`; the ARM64 source list now includes `prog_catalog.c`.
- The catalog model is consumed by the Spatial Minimal Launcher added in UI-3 section 12; the catalog remains independent from the trust manifest.
- Scope: master plan J/J.1, K, F and N; workstreams 2, 5, 9 and 12.


## 12. UI-3 — first integrated Launcher slice (2026-11-13)

- The primary navigation rail now contains the real system destinations in this order: Home, Applications, Files, KIRA AI, Monitor, Settings and Terminal. Applications is a normal navigation destination; its former duplicate top-bar button has been removed. The separate Window Overview control remains in the system zone.
- The bottom Dock remains a projection of the persisted user-pinned application list only. Running-but-unpinned applications and the old “add program” action are not rendered or hit-testable in Spatial Minimal. Existing `/var/dock` persistence and pin/unpin actions remain intact.
- Added a kernel-owned Launcher window that renders the generated curated catalog, supports substring search over program name/title/category/description, keyboard navigation, Enter launch, row selection and Escape close.
- Search input is routed through the shared input layer and Russian keyboard-layout conversion. The Launcher owns keyboard focus only while it is the top visible window; Explorer receives focus only when it is top and Launcher is not. The console is told when either owns the keyboard.
- Launch requests use prog_request() and report accepted/busy/invalid plus asynchronous completion. No direct ELF execution or trust bypass was added; prog_run_ex() remains responsible for manifest/SHA-256 verification.
- Added a WM regression assertion for the Spatial-only launcher callback. Full display-level keyboard/search/launch integration tests are still needed.
- Validation status: the preceding exact code head 37a53ba6229ab7f8a556c6ff93815c5851549b2a passed x86-64, ARM64, host-tests and the VM matrix (run 43388886629). This new Launcher code is later and is NOT covered by that PASS; a fresh CI run is required. No local build, Limbo visual check or physical-hardware test is claimed.


## 11. Android/Limbo preview startup correction (2026-11-13)

- Root cause: `display_init()` always called `wm_init()`, which deliberately selected `WM_LAYOUT_LEGACY`. Spatial Minimal existed but was never selected by the Android build.
- Added `wm_init_mode()`; existing `wm_init()` remains a Legacy-compatible wrapper.
- `build_clang.sh --android` defines `KIRA_SPATIAL_PREVIEW`; only this Android/Limbo build selects Spatial Minimal. Ordinary builds and CI retain Legacy.
- Spatial Minimal startup skips allocation and rendering of the full-screen legacy brain background. Switching back to Legacy builds it lazily.
- Added a dedicated startup-mode host regression test. It does not replace Limbo visual/performance verification.


## 12. Navigation regression and spatial chrome correction (2026-11-13)

- Host display-preview failure was caused by the test still clicking legacy tab indices after the navigation catalog was reordered. Updated the test to activate Mind at index 3 and Files at index 2; Settings/Home retain their indices.
- Spatial Minimal compositor no longer draws the Legacy top bar. This removes the duplicate horizontal navigation/status strip; the primary navigation remains on the left rail and pinned apps remain in the bottom Dock.
- The host suite must pass both the dedicated Spatial Minimal startup smoke test and the full Legacy display-preview interaction sequence. Neither result is inferred from a build-only pass.


## 13. ARM64 GUI scenario marker compatibility fix (2026-11-13)

- Run `43395060732` built x86-64 and ARM64 successfully; all 20 listed VM scenarios passed. ARM64 build itself passed, but `arm64-gui`, `arm64-explorer2`, `arm64-ramfb` and `arm64-dma-nc` failed their startup assertion because the expected literal `[KIRA DISPLAY] Shell online` had been changed to mode-specific strings.
- The job log showed the failure was the missing marker, not a reported kernel/display initialization error. This is a test-contract regression: the shell-ready event must remain mode-independent.
- Fixed both startup variants in `display.c` to retain the stable prefix `[KIRA DISPLAY] Shell online:` and append the selected layout details.
- Fix commit: `613a6e734b767914c14b1d511c8e5677e5d3e246`.
- This fix has not yet been validated by a workflow run on its exact commit. Host-tests were still in progress in run `43395060732`; ARM64 VM scenarios need a fresh run after the fix. No local build or Limbo visual test is claimed.


## 14. Spatial Minimal partial-redraw regression fix (2026-11-13)

- Root cause found in `wm.c`: `damage_side()` marked the full desktop below the top edge dirty in Spatial Minimal. It is called on Dock hover changes and on app/window lifecycle changes. The navigation hover path also reused `damage_side()`, causing unrelated desktop tiles to be recomposed for a rail tooltip.
- Dock invalidation is now limited to the bottom band containing the Dock and its hover label.
- Navigation hover invalidation now targets only the previous/current rail item and its tooltip bounds through `damage_spatial_tab()`.
- Added `tests/ui_damage_contract_test.py` and wired it into `tests/run_host_tests.sh` to prevent full-desktop invalidation from returning.
- This is a source-level performance correction, not a measured performance claim. Exact-head host tests, QEMU/Limbo visual check and frame-time comparison are still required.
- Legacy invalidation path is unchanged. No default-layout switch or ISO release is made by this change.


## 15. Partial-redraw verification update (2026-11-13)

- Exact PR workflow run `43396919980`: x86-64 build PASS; full host suite PASS (`ALL HOST TESTS PASSED`); ARM64 Clang `-Werror` build PASS; ARM64 QEMU scenarios PASS; all 29 VM matrix jobs PASS, including `limbo-spam`.
- The host suite includes `tests/ui_damage_contract_test.py`, so the source-level bounded-damage regression guard executed successfully in CI.
- Still unverified: appearance and interaction on the user's Android Limbo session, real frame-time/dirty-pixel measurements, and physical hardware. CI's `limbo-spam` is not the user's phone emulator.
- PR #140 updated with the requirement/evidence matrix; remains Draft and is not merged. No ISO generated or installed.


## 16. Active navigation state invalidation correction (2026-11-13)

- Follow-up review found one remaining caller in `compose_prepare()`: when the active section changed, it still called `damage_side()`. After the Dock optimization this repainted the Dock band instead of the left rail, so the active-section marker could remain visually stale.
- Updated active-tab changes to invalidate only the previous and current rail item via `damage_spatial_tab()`; Legacy continues to invalidate its top panel.
- Extended `tests/ui_damage_contract_test.py` to assert both hover transitions and active-section transitions use bounded per-item invalidation, and that active-tab changes do not call `damage_side()`.
- Source/test commits: `85884292734df3ae4a4b08888a8768aa835fc682`, `e5234a8c3126898c1bf9dc2feb1b39f9b0f3bcae`.
- Prior run `43396919980` validated the earlier Dock/hover fix only. Fresh exact-head CI is required for this follow-up. Android Limbo visual check and frame-time/dirty-pixel measurements remain open.


## 17. Exact-head CI verification (2026-11-13)

- Exact-head run `43408346184` passed on branch head `1850c0ea0f4f3436116b1b3e3942f5f05293ef34`; source/test head was `e5234a8c3126898c1bf9dc2feb1b39f9b0f3bcae` (later commits are documentation-only).
- Host tests, x86-64 build, ARM64/QEMU and all VM scenarios passed, including `limbo-spam`, `limbo-mttcg`, USB-UHCI, USB-xHCI and driver scenarios.
- This verifies the bounded invalidation contracts and builds. It does not measure FPS/frame time/dirty-pixel reduction and does not replace the user's Android Limbo visual comparison.


## 18. Spatial system-zone visibility correction (2026-11-13)

- Visual-path inspection found that `draw_topbar()` already had a compact Spatial branch (KIRA mark, Window Overview action, clock/date), but `compose_rect()` called it only for Legacy. The Spatial Overview hit target was therefore invisible and the system clock had no visible surface.
- `compose_rect()` now draws the layout-specific top system zone for both modes. Spatial uses only its compact system strip; it does not draw the Legacy horizontal navigation tabs or telemetry meters.
- `tests/wm_test.c` now checks the actual Spatial system-zone pixels and bottom border after switching layouts. Existing hit-test verifies the Overview action remains functional.
- Android/Limbo builds select Spatial Minimal through `build_clang.sh --android` (`KIRA_SPATIAL_PREVIEW`); ordinary CI/desktop builds retain Legacy for compatibility.
- This closes a concrete rendering defect in the preview, not the full visual acceptance gate. The current phone/Limbo appearance, touch ergonomics and frame-time remain unverified.
