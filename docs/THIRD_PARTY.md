# Сторонние компоненты и лицензии Kira OS

Обновлено: 2026-11-18

> **Это инвентаризация для передачи юристу, а не юридическое заключение.** Выводы «можно / можно с условиями / риск / нельзя» - рабочая оценка по тексту лицензий и исходников, не гарантия. Каждое утверждение ниже опирается на источник (указан в скобках). Что не удалось установить - вынесено в раздел 5, не додумано.

Правила проекта: LICENSE в корне - «все права защищены», правообладатель Пастушенко Константин Олегович; рассматривается и вариант «двойная лицензия / открытие под конкурсы» (docs/INDUSTRIAL_READINESS.md, раздел 7, пункт 1). Колонки «Закрытая» и «Открытие» отвечают на вопрос: совместим ли компонент с каждым из двух вариантов. Тексты уведомлений, которые нужно сохранять, лежат в файле NOTICE в корне. Проверка полноты: `python3 ci/check_licenses.py` (раздел 6).

Что считается поставкой: (а) образ `kira.iso` и диск (ядро, программы, `python.wasm`, `ksh.wasm`, `BOOTX64.EFI`, шрифт, обои, корневые сертификаты); (б) архив исходников - `git archive` всего репозитория (ci/package_update.sh, строка 92), поэтому в него попадает всё отслеживаемое git, включая `prebuilt/`, `tests/`, `tools/`, `docs/`.

## 1. Сводка

| Компонент | Где используется | SPDX | Закрытая | Открытие | Вывод |
|---|---|---|---|---|---|
| kira-own: наш код (ядро, sha256, crypto, pk, json, cbor, zstd.c, icons, загрузчики) | образ, архив | LicenseRef-Kira-Proprietary | да | да | можно с условиями |
| micropython 1.24.1 | образ (python.wasm) | MIT | да | да | можно с условиями |
| wasi-libc (musl, cloudlibc, dlmalloc) | образ (внутри python.wasm, ksh.wasm) | Apache-2.0 WITH LLVM-exception / Apache-2.0 / MIT; BSD-2-Clause; CC0-1.0 | да | да | можно с условиями |
| compiler-rt (LLVM builtins) | образ (внутри python.wasm) | Apache-2.0 WITH LLVM-exception | да | да | можно |
| gnu-efi (libgnuefi, libefi, crt0, заголовки) | образ (BOOTX64.EFI) | BSD-3-Clause (HP), BSD-2-Clause (Intel), MIT (efipoint.h) | да | да | можно с условиями |
| unifont (глифы в font8x16.c) | образ | OFL-1.1 или GPL-2.0-or-later с исключением | OFL - да | OFL - да | риск |
| wallpaper (data/wallpaper.kwp*, снимки docs/ui) | образ, архив | неизвестно | неизвестно | неизвестно | риск |
| grub (запасной kira-grub.iso) | образ (только запасной) | GPL-3.0-or-later | только если выполнить GPL для GRUB | да, но GPL-обязательства остаются | риск |
| ca-roots (/etc/ssl/roots.der из системы хозяина) | образ | MPL-2.0 (certdata.txt) | да, с условиями | да | можно с условиями |
| openbsd-permissive (BSD-2/3 файлы корпуса) | не поставляется (скачивается на машине пользователя) | BSD-2-Clause, BSD-3-Clause | да | да | можно с условиями |
| openbsd-greenman (dp8390, ne2000reg) | не поставляется | LicenseRef-Greenman (проектное имя) | да | да | можно с условиями |
| openbsd-4clause (dc, pcn, pcidevs) | не поставляется | BSD-4-Clause (с рекламной оговоркой) | риск | нельзя (несовместимо с GPL) | нельзя |
| kira-bsd-compat (наши заголовки и рантайм под API OpenBSD) | образ драйверов | BSD-2-Clause | да | да | можно с условиями |
| rfc-test-vectors (ключи RFC 8032) | тесты | не установлена | да | да | можно с условиями |
| qemu-acpi (pc_dsdt.aml, q35_dsdt.aml) | архив (тесты) | не установлена (сняты с QEMU, GPL-2.0) | риск | риск | риск |
| ovmf | только тесты | BSD-2-Clause-Patent и др. | не поставляется | не поставляется | можно |
| swtpm | только тесты | BSD-3-Clause | не поставляется | не поставляется | можно |
| qemu | только тесты | GPL-2.0 (по пакету) | не поставляется | не поставляется | можно |
| ipxe-qemu | только CI | GPL-2.0 и др. | не поставляется | не поставляется | можно |
| py-deps (Pillow, cryptography, zstandard) | только инструменты и тесты | MIT-CMU; Apache-2.0 OR BSD-3-Clause; BSD-3-Clause | да | да | можно |
| toolchain (clang, nasm, xorriso, mtools, dosfstools, iasl) | только сборка | разные | не входит в поставку | не входит в поставку | можно с условиями |

Расшифровка вывода: **можно** - никаких действий; **можно с условиями** - надо сохранять уведомления или выполнить оговорку (см. раздел 2); **риск** - решение владельца или замена до поставки; **нельзя** - в поставку не включать.

## 2. По компонентам

### kira-own - наш код
- Источник утверждения: LICENSE; в 1322 файлах исходников SPDX-заголовок есть только у 6 (drivers/net/rtl8139*, kcc/include/bsd/bsd_core.h, kdrv_bsd.h в двух местах); остальные перечислены в ci/license_baseline.txt как «наши, заголовка нет».
- Чужих строк Copyright в исходниках нет (поиск по первым 30 строкам, ci/check_licenses.py).
- **sha256.c, crypto.c (ChaCha20, Poly1305, X25519, AES-GCM, HKDF, SHA-512), pk.c (RSA, ECDSA, Ed25519), json.c, cbor.c, zstd.c**: ни заголовков, ни упоминаний заимствования; комментарии ссылаются на стандарты (RFC 8032, 7748, 8439, 5869; FIPS 197, 180-4; RFC 8878 для zstd). Значит, это реализации по спецификации. Для zstd.c двойной лицензии (BSD/GPLv2) нет: код не помечен как производный от libzstd, поэтому выбирать нечего. Константы XXH64 в zstd.c - из описания алгоритма xxHash. Подтвердить авторство может только владелец (коммиты: ccf17a9c1 для zstd, aeafac55e и ade654719 для остального).
- **icons.c**: рисует значки векторными вызовами ui_seg, внешних данных нет.
- **Условие**: в коммитах стоит соавтор-ИИ (Co-Authored-By: Claude). Вопрос, кому принадлежат права на такой код и можно ли на него поставить «все права защищены», - для юриста.
- **Условие**: новый файл получает первой строкой `SPDX-License-Identifier: LicenseRef-Kira-Proprietary`.

### micropython 1.24.1
- Версия: баннер «MicroPython v1.24.1» в prebuilt/python.wasm; закреплённый коммит ecfdd5d6f9be971852003c2049600dc7b3e2a838 (tools/mpy_kira/pins.txt).
- Лицензия: MIT, Copyright (c) 2013-2024 Damien P. George; «отдельные файлы могут иметь других правообладателей» (LICENSE репозитория MicroPython на этом коммите). Сторонние библиотеки MicroPython в lib/ (axtls, berkeley-db BSD-4-clause, lwip и др.) нужны только при включении соответствующих MICROPY_PY_* (extmod.mk); в tools/mpy_kira/Makefile и mpconfigport.h их включения нет, но состав объектов реальной сборки я не проверял (сборку не запускал).
- Требует: сохранить уведомление об авторских правах и текст MIT при распространении (в NOTICE, раздел 1). Исходники отдавать не нужно, копилефта нет.
- Исходники MicroPython в нашем репозитории не лежат: берутся с GitHub по закреплённому коммиту; в репозитории только наши файлы tools/mpy_kira/.

### wasi-libc (коммит 4ec32c50873163d0d8f863393bc0a1d3a4475c24)
- Лицензия (LICENSE репозитория): Apache-2.0 WITH LLVM-exception, Apache-2.0 и MIT; вложенное: dlmalloc - CC0, emmalloc - MIT, cloudlibc - BSD-2-Clause, musl - MIT (COPYRIGHT musl), musl-fts - BSD-3-Clause. Внутри musl есть части под BSD-2 (TRE), BSD (crypt DES), общественное достояние (blowfish), «MIT-стиль» (qsort) - перечислены в COPYRIGHT musl.
- Что реально линкуется: libc.a и crt1-command.o в python.wasm и ksh.wasm (tools/mpy_kira/Makefile; strings в wasm показывает dlmalloc).
- Требует: в двоичной поставке воспроизвести уведомления MIT/BSD-2 (NOTICE, раздел 2). Копилефта нет.

### compiler-rt builtins (llvm-project, коммит 3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff)
- Лицензия: Apache-2.0 WITH LLVM-exception (compiler-rt/LICENSE.TXT). Исключение прямо разрешает распространять встроенные в объектный код части без выполнения пунктов 4(a), 4(b), 4(d) Apache-2.0. Ссылка в NOTICE (раздел 3) - вежливость, не требование.

### gnu-efi (пакет Ubuntu gnu-efi 3.0.15-1build1; сборка загрузчика)
- Лицензии по Debian/Ubuntu copyright (/usr/share/doc/gnu-efi/copyright): основной код и crt0/libgnuefi - BSD-3-clause (HP), inc/* и lib/* - BSD-2-clause (Intel), inc/efipoint.h - Expat (J. Cronin), inc/aarch64 и arm efibind.h - «BSD-2 Linaro или GPL-2+» (для ARM64 выбирается BSD-2).
- В файле BSD-4-clause-Intel (inc/protocol/efidbg.h, ia64/eficontext.h) наш x86-загрузчик не подключает: проверено препроцессором (clang -E -H на arch/x86_64/uefi/loader.c). Для arch/aarch64/uefi/loader.c проверка не делалась.
- Что линкуется в BOOTX64.EFI: crt0-efi-x86_64.o, libefi.a, libgnuefi.a (build_uefi.sh, строки 84-85); в prebuilt/BOOTX64.EFI видны символы gEfi*Guid и efi_main.
- Требует: воспроизвести уведомления BSD-3/BSD-2 в документации поставки (NOTICE, раздел 4). Имена HP и Intel нельзя использовать для рекламы без разрешения.
- В prebuilt/BOOTX64.EFI версия gnu-efi, которой он собран, не записана (раздел 5).

### unifont - шрифт 8x16 (font8x16.c, font.h)
- Происхождение: заголовок font8x16.c: «растровый шрифт 8x16 из GNU Unifont (GPLv2+ с исключением для встраивания шрифта)»; генератора шрифта и версии Unifont в репозитории нет.
- Официальная страница проекта (https://www.unifoundry.com/unifont): GPL v2 или новее «с исключением: встраивание шрифта в документ само по себе не нарушает GPL»; с версии 13.0.04 шрифты также двойная лицензия SIL OFL 1.1 / GPL-2+.
- Риск: исключение говорит о «документе», а у нас глифы внутри исполняемого кода ядра и программ; при чистом GPL это может потянуть копилефт на ядро. Выход: взять вариант **OFL 1.1** (разрешает встраивание в программы, требует сохранить уведомление и текст OFL, шрифт отдельно не продавать) - но нужна уверенность, что взяты глифы версии 13.0.04 и новее (раздел 5), или заменить шрифт на свой.

### wallpaper - обои и снимки экрана
- data/wallpaper.kwp и .kwp.zst - картинка 1280x720 (формат KWP1), конвертирована tools/mkwallpaper.py. BACKLOG.md называет её «фото» и планирует «нейтральные обои вместо фото». **Происхождение и лицензия фото в репозитории не записаны.** Источник и право на распространение установить нельзя - это риск.
- docs/ui/*.png - снимки экрана Kira OS; на них, по всей видимости, обои и текст шрифтом из раздела unifont (изображения я не открывал).
- Действие: либо подтвердить источник и лицензию (письменно), либо заменить на обои, созданные программно (однотонные или генерируемые), и удалить фото из истории при публичном открытии.

### grub - запасной образ kira-grub.iso (GRUB 2.12 в окружении сборки; не лежит в git)
- Лицензия: GPL-3.0-or-later (Debian copyright пакета grub-common). Образ делает build_clang.sh строки 331-333 командой grub-mkrescue; основной kira.iso собирается нашим загрузчиком без GRUB (строка 330).
- Если распространять kira-grub.iso: GPLv3 требует приложить лицензию, передать или предложить письменно (на 3 года) полный исходный код GRUB (GPLv3 п. 6) и для «пользовательских устройств» - данные для установки изменённой версии (Installation Information, п. 6), что при Secure Boot и «закрытой» лицензии на целом образе трудно. Наше ядро kira.bin лежит на ISO как отдельный файл и загружается по Multiboot2; GPLv3 п. 5 говорит, что простое объединение в «агрегат» лицензию на остальные части не распространяет (локальный текст /usr/share/common-licenses/GPL-3, строки 239-243), но будет ли ISO «агрегатом» - вопрос юристу.
- **Рекомендация: не распространять kira-grub.iso** (оставить внутренним тестовым образом) и не включать GRUB в релиз. Вариант с GPL-обязательствами - только по решению владельца.

### ca-roots - корневые сертификаты
- build_arm64.sh и ci/kira_vm.py копируют системную связку хозяина (/etc/ssl/certs/ca-certificates.crt) в /etc/ssl/roots.der на диск Киры. По Debian copyright пакета ca-certificates файл mozilla/certdata.txt - MPL-2.0.
- Требует (MPL-2.0 - копилефт на уровне файла): сохранить уведомление и сообщить, что данные взяты из набора Mozilla; сами данные - исходный вид, наш код они не затрагивают. Версия набора в образ не записывается (раздел 5).

### openbsd-* - корпус драйверов OpenBSD (коммит 4ce71f2817f675745b0596aaf04de4a122b1cc52)
- В репозитории и образе **нет** файлов OpenBSD: tools/bsd_corpus.py скачивает их с GitHub по хешам из drivers/bsdcorpus/manifest.txt; в drivers/index.txt сегодня одна запись - наш rtl8139. Чужие файлы окажутся только на машине пользователя (с их заголовками лицензий).
- Лицензии каждого из 35 файлов манифеста проверены по их заголовкам на указанном коммите (скачаны с raw.githubusercontent.com, прогнаны через tools/bsd_recipe.py classify_license):
  - **BSD-2-Clause** (23 файла): aic6915.c/.h, ax88190reg/var.h, fxp.c/reg/var.h, if_fxp_pci.c, mtd8xx.c/reg/var.h, if_mtd_pci.c, ne2000.c, ne2000var.h, if_ne_pci.c, rtl80x9.c/reg/var.h, if_lii.c, if_liireg.h, if_sf_pci.c, if_vte.c, if_vtereg.h (правообладатели: NetBSD Foundation, David Greenman, Oleg Safiullin, Pyun YongHyeon и др.).
  - **BSD-3-Clause** (3): am79900reg.h, lancereg.h, microcode/fxp/rcvbundl.h (Intel).
  - **Greenman** (dp8390.c, dp8390reg.h, dp8390var.h, ne2000reg.h): короткая разрешительная лицензия Дэвида Гринмана («может использоваться, изменяться, копироваться, распространяться и продаваться при сохранении уведомления»). У dp8390.c и dp8390var.h добавлена строка Copyright Charles M. Hannum «All rights reserved» без отдельных условий - это неоднозначно. tools/bsd_recipe.py на этих файлах отказывает (см. раздел 5), а userland/drvget_core.c принимает.
  - **С рекламной оговоркой (BSD-4-Clause)**: dc.c, dcreg.h, if_dc_pci.c (Bill Paul), if_pcn.c (Wasabi Systems для проекта NetBSD), pcidevs (Christopher G. Demetriou для NetBSD). Оговорка: «во всех рекламных материалах, упоминающих возможности или использование ПО, должна быть надпись: This product includes software developed by ...». Проект сам их отвергает (ci и drvget_core: «рекламная оговорка вне допустимого набора»). **Заголовок manifest.txt «BSD-лицензия» для этих файлов неточен.** pcidevs при этом нужен для генерации kd_pcidevs.h (docs/BSD_COMPAT.md) - таблица идентификаторов устройств; можно ли считать производной таблицу фактов - вопрос юристу.
- Требует (BSD-2/3): сохранять заголовки в исходниках и воспроизвести уведомление при двоичной поставке; имена правообладателей не использовать для рекламы (BSD-3). Копилефта нет.

### kira-bsd-compat и rtl8139
- kcc/include/bsd/*, userland/kdrv_bsd.h, kcc/include/kdrv_bsd.h, userland/bsd_rt.c - наши; SPDX BSD-2-Clause только у bsd_core.h и kdrv_bsd.h, остальные заголовки - однострочные обёртки над bsd_core.h; чужих Copyright нет.
- drivers/net/rtl8139*: «Copyright (c) Kira OS authors. Original implementation from the public Realtek RTL8139 programming specification», SPDX BSD-3-Clause, рецепт drvget.rec подтверждает «без заимствований из BSD/Linux».
- Оговорка: имена и значения констант (ifmedia, MII, pcireg) совпадают с OpenBSD по необходимости совместимости API; копирования файлов не выявлено, но и не исключено (раздел 5).

### rfc-test-vectors
- Тестовые ключи Ed25519 «TEST-ED25519-001/002» (kdrv_trust.h) - векторы RFC 8032; сборка с ними запрещена вне тестов (#error при KDRV_TEST_TRUST без KIRA_TEST_BUILD). Лицензия текста RFC и порядок использования векторов (IETF Trust) мной не проверялись (раздел 5). Образ kira-bios-signedapps.img - наш тестовый образ с тестовым доверием: в релиз не входит.
- keys/KIRA_OWNER_ED25519_PUBLIC.hex - открытый ключ владельца, наш.

### qemu-acpi
- tests/data/acpi/pc_dsdt.aml и q35_dsdt.aml - «настоящие DSDT из QEMU» (сообщение коммита e89c0b79a, OEM-метка BOCHS/BXPC). QEMU распространяется под GPL-2.0 (Debian copyright qemu-system-common ссылается на GPL-2). Считаются ли таблицы, сгенерированные QEMU, производными от него - не установлено. Файлы только тестовые, но лежат в архиве исходников.
- Действие: заменить собственными таблицами из .asl (как laptop.asl, собран iasl) или убрать из публичного архива.
- laptop.asl/.aml - наши; в tests/data/acpi/build.sh пересборка через iasl (лицензия iasl не проверялась).

### ovmf, swtpm, qemu, ipxe-qemu - тестовые инструменты
- Не копируются в образ или архив (поиск по ci/ и tests/: используются только как запускаемые программы - ci/kira_vm.py, ci/package_update.sh берёт только серийный лог OVMF). Лицензии пакетов Ubuntu: ovmf 2024.02 - BSD-2-Clause-Patent, BSD-3-clause, OpenSSL, а также GPL-2+/GPL-3 в отдельных частях дерева; swtpm - BSD-3-clause; qemu - GPL-2 (ссылка в copyright пакета); ipxe-qemu - GPL-2 и др.
- Условие: пока они не включены в поставку и не лежат в репозитории, обязательств нет. Если решите поставлять образы с OVMF или готовой виртуальной машиной - это новая поставка со своими требованиями (OVMF содержит OpenSSL и GPL-части).

### py-deps и toolchain
- Python-зависимости (по метаданным установленных пакетов): Pillow 12.3.0 - MIT-CMU; cryptography 50.0.1 - Apache-2.0 OR BSD-3-Clause; zstandard 0.25.0 - BSD-3-Clause. Нужны tools/mkwallpaper.py и тестам; в образ не входят.
- Инструменты сборки: clang 18.1.3 (Apache-2.0 WITH LLVM-exception по LLVM LICENSE.TXT), nasm 2.16.01, xorriso (GPL-2+/GPL-3 по Debian copyright), mtools, dosfstools, iasl. Не поставляются. Что результат работы GPL-инструмента не наследует лицензию, по официальным текстам я не проверял (gnu.org недоступен из среды), для nasm, mtools, dosfstools и iasl лицензии не сверялись.

## 3. Сводка рисков и что делать

| № | Риск | Что сделать |
|---|---|---|
| Р1 | Обои: источник и лицензия фото неизвестны (data/wallpaper.kwp*, снимки docs/ui) | Подтвердить источник письменно или заменить обоями, созданными программно; снимки пересделать |
| Р2 | kira-grub.iso: GPL-3.0+, обязательства при распространении (исходники GRUB или письменное предложение, лицензия, данные для установки) | Не распространять этот образ; в релиз - только kira.iso с нашим загрузчиком |
| Р3 | Шрифт: происхождение (Unifont), версия неизвестна; GPL-исключение говорит о «документах» | Подтвердить версию 13.0.04+ и взять OFL 1.1 (приложить текст) либо заменить своим шрифтом (интерфейс font.h сохраняется) |
| Р4 | Файлы OpenBSD с рекламной оговоркой (dc, pcn, pcidevs) и неоднозначные Greenman/Hannum | Не включать dc и pcn в каталог поставки; pcidevs - решение юриста или генерация таблицы из открытого источника с ясной лицензией; исправить заголовок manifest.txt; для dp8390 запросить разъяснение или не поставлять ne2k |
| Р5 | Тестовые таблицы ACPI, снятые с QEMU (GPL-2.0) лежат в архиве исходников | Заменить своими (.asl) или убрать из публичного архива |
| Р6 | Права на код, написанный с участием ИИ; подтверждение авторства sha256/crypto/pk/zstd | Решение юриста; владельцу письменно зафиксировать авторство |
| Р7 | Двоичный prebuilt/BOOTX64.EFI и python.wasm не воспроизводимо привязаны к версиям gnu-efi и sysroot | Записывать версии при сборке; приложить NOTICE к каждой поставке |
| Р8 | Корневые сертификаты MPL-2.0: версия набора не фиксируется | Записывать версию ca-certificates в манифест релиза |

Для варианта «двойная лицензия / открытие»: несовместимых с открытием копилефт-файлов в образе нет, кроме GRUB (Р2) и спорных Р1, Р3; 4-clause файлы OpenBSD (Р4) с GPL несовместимы.

## 4. Что нужно приложить к поставке
- NOTICE из корня - к каждому образу и архиву.
- Тексты лицензий Apache-2.0 (LLVM) и OFL 1.1 (если останется Unifont) - к NOTICE (в NOTICE даны ссылки; OFL текст не воспроизведён).
- Исходники MicroPython и wasi-libc отдавать не нужно (MIT/BSD/Apache без копилефта), но закреплённые коммиты зафиксированы (tools/mpy_kira/pins.txt).

## 5. Что не удалось установить
- Происхождение и лицензия фото обоев; происхождение 8 снимков docs/ui (изображения не просматривались).
- Версия Unifont и генератор font8x16.c; наличие у выбранной версии варианта OFL (официальная страница показывает 13.0.04+; какая версия взята - неизвестно).
- Версия gnu-efi, из которой собран prebuilt/BOOTX64.EFI; проверка aarch64-загрузчика на файлы BSD-4-clause.
- Состав объектов реальной сборки python.wasm (сборку не запускал): вывод о MIT-составе опирается на Makefile и mpconfigport.h.
- Лицензия и порядок использования тестовых векторов RFC 8032 (IETF Trust); лицензии nasm, mtools, dosfstools, iasl, GitHub Actions; содержимое tests/data/zstd (сжатые файлы сделаны утилитой zstd, источник данных text_*/struct_* не записан); происхождение data/mind_seed.bin (440 байт).
- gnu.org и unifoundry.com из среды недоступны напрямую: текст GPLv3 взят из локального /usr/share/common-licenses, страница Unifont - через WebFetch.
- tools/bsd_recipe.py classify_license отказывает на dp8390.c, dp8390reg.h, dp8390var.h, ne2000reg.h (проверено запуском), хотя в нём есть ветка LicenseRef-Greenman; причина (строки-переносы текста) не разбиралась.

## 6. Как поддерживать
`python3 ci/check_licenses.py` (запускается в .github/workflows/docs.yml) падает, если: у нового файла исходников нет SPDX-заголовка и он не внесён в ci/license_baseline.txt; файл с чужим SPDX или чужим Copyright, либо новый бинарник/данные не перечислены ниже; запись ниже устарела; закреплённый коммит не упомянут в этом файле; у компонента с notice=yes нет имени в NOTICE. Самопроверка: `python3 ci/test_check_licenses.py`.

Новый сторонний файл: добавьте строку в блок files (с компонентом из components), опишите компонент в разделах 1-2, добавьте уведомление в NOTICE.

## Приложение: машинные списки (читает ci/check_licenses.py)

Компоненты: id | SPDX | область | вывод | уведомление в NOTICE

```text
# licenses:components
kira-own | LicenseRef-Kira-Proprietary | образ | можно с условиями | no
micropython | MIT | образ | можно с условиями | yes
wasi-libc | Apache-2.0 WITH LLVM-exception AND MIT AND BSD-2-Clause AND CC0-1.0 | образ | можно с условиями | yes
compiler-rt | Apache-2.0 WITH LLVM-exception | образ | можно | yes
gnu-efi | BSD-3-Clause AND BSD-2-Clause AND MIT | образ | можно с условиями | yes
unifont | OFL-1.1 OR GPL-2.0-or-later | образ | риск | yes
wallpaper | неизвестно | образ | риск | no
grub | GPL-3.0-or-later | образ | риск | no
ca-roots | MPL-2.0 | образ | можно с условиями | no
openbsd-permissive | BSD-2-Clause AND BSD-3-Clause | не поставляется | можно с условиями | no
openbsd-greenman | LicenseRef-Greenman | не поставляется | можно с условиями | no
openbsd-4clause | BSD-4-Clause | не поставляется | нельзя | no
kira-bsd-compat | BSD-2-Clause | образ | можно с условиями | no
rfc-test-vectors | неизвестно | тесты | можно с условиями | no
qemu-acpi | неизвестно | архив | риск | no
ovmf | BSD-2-Clause-Patent | тесты | можно | no
swtpm | BSD-3-Clause | тесты | можно | no
qemu | GPL-2.0-only | тесты | можно | no
ipxe-qemu | GPL-2.0-only | тесты | можно | no
py-deps | MIT-CMU AND Apache-2.0 AND BSD-3-Clause | сборка | можно | no
toolchain | разные | сборка | можно с условиями | no
```

Файлы: glob | класс (own, third, risk) | SPDX | компонент. Исходники без SPDX сюда не вносятся (они в ci/license_baseline.txt).

```text
# licenses:files
drivers/net/rtl8139* | own | BSD-3-Clause | kira-own
kcc/include/bsd/bsd_core.h | own | BSD-2-Clause | kira-bsd-compat
kcc/include/kdrv_bsd.h | own | BSD-2-Clause | kira-bsd-compat
userland/kdrv_bsd.h | own | BSD-2-Clause | kira-bsd-compat
font8x16.c | third | OFL-1.1 OR GPL-2.0-or-later | unifont
data/wallpaper.kwp | risk | неизвестно | wallpaper
data/wallpaper.kwp.zst | risk | неизвестно | wallpaper
docs/ui/*.png | risk | неизвестно | wallpaper
prebuilt/BOOTX64.EFI | third | BSD-3-Clause AND BSD-2-Clause | gnu-efi
prebuilt/python.wasm | third | MIT AND Apache-2.0 WITH LLVM-exception AND BSD-2-Clause | micropython
prebuilt/ksh.wasm | third | MIT AND BSD-2-Clause | wasi-libc
prebuilt/*.sha256 | own | LicenseRef-Kira-Proprietary | kira-own
tests/data/acpi/pc_dsdt.aml | risk | неизвестно | qemu-acpi
tests/data/acpi/q35_dsdt.aml | risk | неизвестно | qemu-acpi
tests/data/acpi/laptop.* | own | LicenseRef-Kira-Proprietary | kira-own
tests/data/zstd/* | own | LicenseRef-Kira-Proprietary | kira-own
keys/*.hex | own | LicenseRef-Kira-Proprietary | rfc-test-vectors
kira-bios-signedapps.img | own | LicenseRef-Kira-Proprietary | rfc-test-vectors
data/mind_seed.bin | own | LicenseRef-Kira-Proprietary | kira-own
grub.cfg | own | LicenseRef-Kira-Proprietary | grub
drivers/bsdcorpus/*.txt | own | LicenseRef-Kira-Proprietary | openbsd-permissive
drivers/index.txt | own | LicenseRef-Kira-Proprietary | kira-own
drivers/net/drvget.rec | own | BSD-3-Clause | kira-own
tools/mpy_kira/Makefile | own | LicenseRef-Kira-Proprietary | micropython
tools/mpy_kira/pins.txt | own | LicenseRef-Kira-Proprietary | wasi-libc
LICENSE | own | LicenseRef-Kira-Proprietary | kira-own
NOTICE | third | разные (тексты сторонних уведомлений) | micropython
.gitignore | own | LicenseRef-Kira-Proprietary | kira-own
apps.manifest | own | LicenseRef-Kira-Proprietary | kira-own
.github/workflows/*.yml | own | LicenseRef-Kira-Proprietary | kira-own
ci/fixtures/* | own | LicenseRef-Kira-Proprietary | kira-own
ci/*.json | own | LicenseRef-Kira-Proprietary | kira-own
ci/license_baseline.txt | own | LicenseRef-Kira-Proprietary | kira-own
tests/fixtures/* | own | LicenseRef-Kira-Proprietary | kira-own
tests/kcc/* | own | LicenseRef-Kira-Proprietary | kira-own
tests/klink/* | own | LicenseRef-Kira-Proprietary | kira-own
tests/ksh/* | own | LicenseRef-Kira-Proprietary | kira-own
tests/*_expected.txt | own | LicenseRef-Kira-Proprietary | kira-own
tests/sha_vectors.txt | own | LicenseRef-Kira-Proprietary | kira-own
docs/architecture/* | own | LicenseRef-Kira-Proprietary | kira-own
```
