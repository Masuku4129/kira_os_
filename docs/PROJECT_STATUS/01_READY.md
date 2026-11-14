# 01 — Готово / текущая базовая поставка

**Снимок baseline:** main @ 811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4  
**Файлов в baseline:** 829

Здесь «готово» означает: код уже находится в текущей основной линии и является частью текущего исходного baseline. Это **не** заявление о полной завершённости всех функций или о проверке физического железа.

## Что штатная x86-64 CI-сборка реально публикует

- kira.iso — загрузочный образ.
- kira.disk — отдельный системный диск KiraFS с пользовательскими программами и данными.
- kira.bin — бинарный образ ядра.

Для Limbo x86 ISO и системный диск — разные файлы; один ISO не содержит всего пользовательского системного диска.

## Что собирается, но не публикуется как образ в текущем main CI

- kira-arm64.img
- kira-arm64.disk

build_arm64.sh собирает ARM64 ядро и диск, но текущий .github/workflows/ci.yml публикует из ARM64 job результаты сценариев ci-out/arm64, а не сами два образа. Поэтому ARM64 сборочный путь есть, но готовая ARM64 поставка в стандартном CI-артефакте не подтверждена.

## Базовые подсистемы, уже присутствующие в main build

| Область | Основные файлы | Фактический статус |
|---|---|---|
| Ядро и процессы | kernel.c, syscall.c, proc.c, sched.c, percpu.c, futex.c, kobj.c | В baseline и x86/ARM build lists; полный аудит lifecycle ещё нужен |
| Память | pmm.c, vmm.c, heap.c, uaccess.c, ramext.c | В baseline; hardware/конкурентные сценарии проверяются отдельно |
| x86-64 HAL | arch/x86_64/ | В x86 build; не равнозначно физической аппаратной приёмке |
| ARM64 HAL | arch/aarch64/, fdt.c | Есть отдельная сборка; реальные ARM-платы не проверены |
| Хранение | kfs.c, vfs.c, fat.c, part.c, blk.c, store.c | В build; транзакционный updater/recovery — отдельный незакрытый пункт |
| Сеть | net.c, ip6.c, ipfrag.c, virtio_net.c, e1000.c | Базовый стек и драйверы в сборке; не все физические адаптеры |
| USB/звук | usb.c, xhci.c, uhci.c, usbhid.c, hda.c, ac97.c, sb16.c, audio.c | В baseline/build; аппаратная матрица неполная |
| GUI | display.c, wm.c, gfxui.c, dock.c, apps.c, explorer.c, fm.c | Текущая оболочка входит в baseline; Spatial redesign остаётся отдельной веткой |
| KIRA Mind | mind.c, mind_sys.c, mindnet.c, data/mind_seed.bin | Реальные компоненты присутствуют; не означает завершённую автономную AI-архитектуру |
| Runtime/toolchain | userland/wasm.c, kcc/, userland/kcc.c, userland/drvget.c, userland/drvman.c | WASM/KCC/Driver Manager присутствуют; их ограничения перечислены в WORKLOG и мастер-плане |

## Файлы, которые автоматически контролируются

Машиночитаемый manifest содержит:
- полный список 829 файлов baseline;
- 98 общих/x86 kernel source inputs;
- 16 x86-64 architecture source inputs;
- 88 ARM64 kernel source inputs;
- списки пользовательских программ, перечисленных в обеих сборках.

Проверка ci/check_project_status.py сверяет эти списки с исходными build scripts и блокирует случайное исчезновение файла или добавление неучтённого файла.

## Ограничения текущей поставки

- Успешная сборка не доказывает полную функциональность драйвера.
- QEMU/Limbo не доказывает работу на физическом x86-64 или ARM64 устройстве.
- В main CI нет опубликованного ARM64 disk/image artifact.
- В main нет завершённого атомарного updater/recovery-пакета. Скрипты release/update, найденные в v46, пока учитываются как branch-only работа.
- «Готово» здесь — включённый baseline, а не безусловный PASS по каждому пункту мастер-плана.
