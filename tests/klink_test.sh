#!/bin/bash
# Тесты компоновщика klink (этап B2 docs/BUILDER_PLAN.md). Запуск из корня: tests/klink_test.sh   (KLINK_QUICK=1 - без пересборки ядра с --gc-sections).
# Эталон - ld.lld с теми же опциями, что в build_clang.sh. ld.lld -O0 не сливает строки (SHF_MERGE), поэтому образ должен совпасть с klink
# по точке входа, программным заголовкам, секциям, байтам сегментов и таблице символов; ld.lld по умолчанию (-O1) сливает одинаковые строки -
# klink этого не делает (ограничение), отличие размеров печатается числами.
# GOT: для нерелаксируемых R_X86_64_GOTPCREL[X] klink заводит ячейку в синтезированной .got (RW, в одном PT_LOAD с .data, как у ld.lld);
# `mov sym@GOTPCRELX(%rip), reg` определённого символа по-прежнему релаксируется в lea.
# Не поддерживается klink (проверяется негативными тестами): .a-архивы, SHT_REL, COMDAT-группы, TLS, R_X86_64_GOT32, PLT/динамическая компоновка, слияние строк, оставшиеся команды скриптов (MEMORY, AT, SORT, ...).
# Пропуски помечаются строкой SKIP с причиной. Загрузка ядра в QEMU: Kernel core starting ... Mind online (как tests/kernel_kcc_boot_test.sh).
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
export ASAN_OPTIONS=detect_leaks=0
pass=0; fail=0; skip=0
ok() { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }
sk() { echo "SKIP: $1 ($2)"; skip=$((skip+1)); }
have() { command -v "$1" >/dev/null 2>&1; }
NPROC=$(nproc 2>/dev/null || echo 2)
HOSTCC=${HOSTCC:-gcc}
have "$HOSTCC" || HOSTCC=clang
have "$HOSTCC" || { echo "FAIL: нужен gcc или clang для сборки klink"; exit 1; }

echo "== сборка klink =="
$HOSTCC -std=c99 -O2 -Wall -Wextra -Werror -o $T/klink klink/klink.c klink/main.c && ok "klink собран ($HOSTCC -std=c99 -Wall -Wextra -Werror)" || { bad "klink не собран"; exit 1; }
KL=$T/klink
CLANG="clang --target=x86_64-unknown-none-elf"
HAVE_TOOLCHAIN=1
for t in clang ld.lld nasm python3; do have $t || { HAVE_TOOLCHAIN=0; sk "сверка с ld.lld" "нет $t"; }; done
if have clang; then
    clang --target=x86_64-unknown-none-elf -ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector \
        -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror -nostdlibinc -Iuserland/kccrt -c klink/klink.c -o $T/klink_kira.o 2>$T/err \
        && ok "ядро klink собирается флагами программ Киры (-ffreestanding -nostdlibinc -Iuserland/kccrt): зависит только от mem*/str*/vsnprintf/setjmp" \
        || bad "klink.c не собирается флагами программ Киры: $(head -c 200 $T/err)"
else
    sk "сборка ядра klink флагами Киры" "нет clang"
fi
if $HOSTCC -std=c99 -O1 -o $T/kcc0 kcc/*.c 2>/dev/null; then
    $T/kcc0 -c -Ikcc/include -Iuserland/kccrt -o $T/klink_kcc.o klink/klink.c 2>$T/err && ok "klink.c собирается собственным kcc (kcc -c)" || bad "kcc не собрал klink.c: $(head -c 200 $T/err)"
else
    sk "kcc" "kcc не собрался"
fi

if [ $HAVE_TOOLCHAIN = 1 ]; then
CMP="python3 tests/klink/cmp_elf.py"
ULINK="-T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro"
KLD="-T arch/x86_64/linker.ld -nostdlib -z max-page-size=0x1000"
$HOSTCC -std=gnu99 -O1 -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c 2>/dev/null && HAVE_LOADER=1 || { HAVE_LOADER=0; sk "запуск под kloader" "kloader не собрался"; }

# --- 1. userland: все программы build_clang.sh ---
echo "== userland: программы build_clang.sh, klink против ld.lld -O0 =="
ubuild() {   # ubuild каталог доп.флаги
    local D=$1 EXTRA=$2
    mkdir -p $D; : > $D/progs.txt
    local UC="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra $EXTRA"
    local US="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -msse2 -mfpmath=sse -mno-mmx -fno-math-errno -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra $EXTRA"
    local jobs=$D/jobs.txt; : > $jobs
    J() { echo "$CLANG $*" >> $jobs; }
    P() { echo "$1|$2" >> $D/progs.txt; }
    for app in plasma paint burn mt ipc serve melody udpecho rec atk-cap; do J $UC -c userland/$app.c -o $D/$app.o; P $app "$D/$app.o"; done
    J $UC -I. -Iuserland -c userland/fetch.c -o $D/fetch.o
    J $UC -I. -Iuserland -c userland/tls.c -o $D/u_tls.o
    J $UC -I. -Iuserland -c userland/x509.c -o $D/u_x509.o
    J $UC -I. -c pk.c -o $D/u_pk.o
    J $UC -I. -c crypto.c -o $D/u_crypto.o
    J $UC -I. -c sha256.c -o $D/u_sha256.o
    J $UC -I. -c kdrv_sig.c -o $D/u_kdrv_sig.o
    J $UC -c userland/libc_min.c -o $D/u_libc_min.o
    P fetch "$D/fetch.o $D/u_tls.o $D/u_x509.o $D/u_pk.o $D/u_crypto.o $D/u_sha256.o $D/u_libc_min.o"
    J $UC -I. -Iuserland -c userland/net6.c -o $D/net6.o
    J $UC -I. -c ip6.c -o $D/u_ip6.o
    P net6 "$D/net6.o $D/u_ip6.o"
    local KO=""
    for src in kcc/*.c; do
        [ "$src" = kcc/main.c ] && continue
        J $UC -nostdlibinc -Iuserland/kccrt -c $src -o $D/kcc_$(basename $src .c).o
        KO="$KO $D/kcc_$(basename $src .c).o"
    done
    J $UC -nostdlibinc -Iuserland/kccrt -Iuserland -c userland/kcc.c -o $D/kcc.o
    J $UC -nostdlibinc -Iuserland -c userland/kccrt.c -o $D/u_kccrt.o
    # режим @build (задание на сборку): те же объекты и порядок, что в build_clang.sh
    J $UC -nostdlibinc -I. -Iuserland/kccrt -c mind_build.c -o $D/u_mbj_build.o
    J $UC -nostdlibinc -I. -Iuserland/kccrt -c mind_build_core.c -o $D/u_mbj_core.o
    J $UC -I. -c elf.c -o $D/u_elf.o
    P kcc "$D/kcc.o $D/u_mbj_build.o $D/u_mbj_core.o $D/u_elf.o $D/u_sha256.o $KO $D/u_kccrt.o $D/u_libc_min.o"
    J $UC -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvget.c -o $D/drvget.o
    J $UC -I. -c userland/drvget_core.c -o $D/u_drvget_core.o
    J $UC -I. -c kdrv_fmt.c -o $D/u_kdrv_fmt.o
    P drvget "$D/drvget.o $D/u_drvget_core.o $D/u_kdrv_fmt.o $KO $D/u_kccrt.o $D/u_tls.o $D/u_x509.o $D/u_pk.o $D/u_crypto.o $D/u_sha256.o $D/u_kdrv_sig.o $D/u_libc_min.o"
    J $UC -nostdlibinc -I. -Iuserland -Iuserland/kccrt -DDRVGET_LIB -c userland/drvget.c -o $D/drvget_lib.o
    J $UC -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvman.c -o $D/drvman.o
    J $UC -I. -c font8x16.c -o $D/font_drvman.o
    P drvman "$D/drvman.o $D/drvget_lib.o $D/u_drvget_core.o $D/u_kdrv_fmt.o $D/font_drvman.o $KO $D/u_kccrt.o $D/u_tls.o $D/u_x509.o $D/u_pk.o $D/u_crypto.o $D/u_sha256.o $D/u_kdrv_sig.o $D/u_libc_min.o"
    for app in cube3d fpu wasm; do J $US -c userland/$app.c -o $D/$app.o; P $app "$D/$app.o"; done
    for app in edit view; do J $UC -I. -c userland/$app.c -o $D/$app.o; J $UC -I. -c font8x16.c -o $D/font_$app.o; P $app "$D/$app.o $D/font_$app.o"; done
    for app in demo atk-mem atk-priv atk-sys atk-gs; do nasm -f elf64 userland/$app.s -o $D/$app.o || return 1; P $app "$D/$app.o"; done
    xargs -P $NPROC -I{} sh -c '{}' < $jobs
}
if ubuild $T/u "" 2>$T/err; then
    n=0; nbad=0
    while IFS='|' read name objs; do
        ld.lld $ULINK -O0 -o $T/u/$name.lld $objs 2>$T/err || { bad "ld.lld не собрал $name"; continue; }
        if ! $KL $ULINK -o $T/u/$name.kl $objs 2>$T/err; then bad "klink не собрал $name: $(head -c 200 $T/err)"; nbad=$((nbad+1)); continue; fi
        if $CMP $T/u/$name.lld $T/u/$name.kl --syms >$T/err 2>&1; then n=$((n+1)); else bad "$name: образ klink отличается от ld.lld: $(head -c 300 $T/err)"; nbad=$((nbad+1)); fi
    done < $T/u/progs.txt
    [ $nbad = 0 ] && ok "$n из $(wc -l < $T/u/progs.txt) программ userland: вход, PT_LOAD (адреса, размеры, байты), секции и таблица символов совпали с ld.lld -O0"
    # elf_parse ядра (elf.c) принимает каждый образ, а запуск даёт тот же вывод, что у ld.lld
    if [ $HAVE_LOADER = 1 ]; then
        nrun=0; nr_bad=0
        while IFS='|' read name objs; do
            a=$(timeout 15 $T/kloader $T/u/$name.lld </dev/null 2>&1 | sed 's/[0-9]\+/N/g'; echo "rc=${PIPESTATUS[0]}")
            b=$(timeout 15 $T/kloader $T/u/$name.kl </dev/null 2>&1 | sed 's/[0-9]\+/N/g'; echo "rc=${PIPESTATUS[0]}")
            case "$b" in *"elf_parse"*) bad "$name: elf_parse ядра отверг образ klink: $b"; nr_bad=$((nr_bad+1)); continue ;; esac
            if [ "$a" = "$b" ]; then nrun=$((nrun+1)); else bad "$name: вывод под kloader отличается (числа заменены на N)"; nr_bad=$((nr_bad+1)); fi
        done < $T/u/progs.txt
        [ $nr_bad = 0 ] && ok "$nrun программ: elf_parse принял, вывод и код возврата под kloader совпали с ld.lld (счётчики и времена нормализованы)"
        # kcc.elf, собранный klink, компилирует программы и самого себя: результат совпадает с kcc.elf от ld.lld байт в байт
        SB=$T/sb; INC=$T/inc; mkdir -p $SB $INC
        cp kcc/include/* tests/kcc/inc/* $INC/
        cp tests/kcc/exec/x_calls.c tests/kcc/exec/x_printf.c tests/kcc/exec/x_data.c $SB/
        good=1
        for t in x_calls x_printf x_data; do
            for p in lld kl; do
                rm -f $SB/o.elf
                $T/kloader -s $SB -i $INC -a "$t.c o.elf" $T/u/kcc.$p >/dev/null 2>&1 && cp $SB/o.elf $SB/$t.$p.elf 2>/dev/null || good=0
            done
            cmp -s $SB/$t.lld.elf $SB/$t.kl.elf || good=0
            [ "$($T/kloader $SB/$t.kl.elf 2>&1 | tail -1)" = "$($T/kloader $SB/$t.lld.elf 2>&1 | tail -1)" ] || good=0
        done
        [ $good = 1 ] && ok "kcc.elf (klink) компилирует x_calls/x_printf/x_data под kloader: результаты совпали с kcc.elf (ld.lld) байт в байт, программы дают тот же код выхода" || bad "kcc.elf (klink) компилирует иначе, чем kcc.elf (ld.lld)"
        if [ -f kcc/selfhost/unity.c ]; then
            SH=$T/sh; mkdir -p $SH/sb $SH/inc
            for f in kcc/*.c kcc/*.h; do [ "$f" = kcc/main.c ] || cp $f $SH/sb/; done
            cp kcc/selfhost/* $SH/sb/; cp userland/kccrt.c userland/kira.h $SH/sb/
            cp mind_build.h mind_build.c mind_build_core.c elf.h immune.h sha256.c $SH/sb/; cp elf.c $SH/sb/rootelf.c      # режим @build (см. kcc/selfhost/unity.c)
            sed 's|"../kcc/kcc.h"|"kcc.h"|; s|"../mind_build.h"|"mind_build.h"|' userland/kcc.c > $SH/sb/kcc_main.c
            sed -i 's|"kcc/kcc.h"|"kcc.h"|' $SH/sb/mind_build.c
            sed -i 's|"kccrt/\([a-z]*\.h\)"|<\1>|' $SH/sb/kccrt.c
            cp kcc/include/* $SH/inc/; cp userland/kccrt/* $SH/inc/
            for p in lld kl; do $T/kloader -m 512 -s $SH/sb -i $SH/inc -a "unity.c st_$p.elf" $T/u/kcc.$p >$SH/log.$p 2>&1; done
            if [ -s $SH/sb/st_kl.elf ] && cmp -s $SH/sb/st_lld.elf $SH/sb/st_kl.elf; then ok "самосборка: kcc.elf (klink) собрал kcc целиком (unity.c, $(stat -c%s $SH/sb/st_kl.elf) байт) - побайтно как kcc.elf (ld.lld)"; else bad "самосборка kcc.elf (klink) не совпала"; fi
        else
            sk "самосборка kcc под kloader" "нет kcc/selfhost/unity.c"
        fi
    fi
else
    bad "не собрались объекты userland: $(head -c 300 $T/err)"
fi

# --- 2. составная тестовая программа: clang + nasm + kcc, слабые/общие символы, GOT, nasm-данные ---
echo "== тестовая программа tests/klink/t_*: несколько объектов =="
mkdir -p $T/t
TU="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Iuserland"
build_t() {   # build_t каталог флаги: объекты clang
    local D=$1 F=$2
    mkdir -p $D
    for f in t_main t_a t_b; do $CLANG $TU $F -c tests/klink/$f.c -o $D/$f.o || return 1; done
    for f in t_c t_d; do $CLANG $TU $F -fcommon -c tests/klink/$f.c -o $D/$f.o || return 1; done
    $CLANG -ffreestanding -fPIE -mno-red-zone -mno-sse -O2 $F -c tests/klink/t_g.c -o $D/t_g.o || return 1
    $CLANG -ffreestanding -fno-pic -fno-pie -mno-red-zone -mno-sse -O2 $F -c tests/klink/t_w.c -o $D/t_w.o || return 1
    nasm -f elf64 tests/klink/t_n.asm -o $D/t_n.o
    nasm -f elf64 tests/klink/t_got.asm -o $D/t_got.o
}
TOBJ="t_main t_a t_b t_c t_d t_g t_w t_n t_got"
objs_of() { local D=$1; shift; for f in "$@"; do echo -n "$D/$f.o "; done; }
if build_t $T/t ""; then
    readelf -r $T/t/t_g.o | grep -q GOTP && ok "t_g.o содержит R_X86_64_[REX_]GOTPCRELX (путь релаксации проверяется)" || bad "t_g.o без GOTPCRELX: тест GOT ничего не проверяет"
    O=$(objs_of $T/t $TOBJ)
    ld.lld $ULINK -O0 -o $T/t/p.lld $O && $KL $ULINK -o $T/t/p.kl $O && $CMP $T/t/p.lld $T/t/p.kl --syms && ok "t_*: образ совпал с ld.lld -O0" || bad "t_*: образ klink отличается от ld.lld"
    if [ $HAVE_LOADER = 1 ]; then
        $T/kloader $T/t/p.kl 2>&1 | head -n -1 > $T/t/out.kl
        diff -q $T/t/out.kl tests/klink/t_expected.txt >/dev/null && ok "t_*: вывод под kloader равен ожидаемому (слабый pick=2, общий cm=5/12, GOT: релаксация и ячейки, указатели в .data, nasm)" || bad "t_*: неверный вывод: $(diff $T/t/out.kl tests/klink/t_expected.txt | head -3)"
    fi
    # .got: по одной ячейке на символ (data_val - общая для двух функций, asm_twice, unused_b_data), в одном RW PT_LOAD с .data, перед .bss; результат детерминирован
    gsz=$(readelf -SW $T/t/p.kl | sed 's/\[ *\([0-9]*\)\]/\1/' | awk '$2==".got"{print $6}')
    $KL $ULINK -o $T/t/p_again.kl $O && cmp -s $T/t/p.kl $T/t/p_again.kl && [ "$gsz" = 000018 ] \
        && readelf -lW $T/t/p.kl | grep -Eq '^ +[0-9]+ +\.data \.got \.bss' \
        && ok "t_*: .got на 3 ячейки (24 байта) в сегменте данных до .bss, повторная линковка даёт тот же образ" || bad "t_*: .got неверен (размер '$gsz') или образ недетерминирован"
    # порядок объектов: сильное определение pick выигрывает у слабого и до него
    O2=$(objs_of $T/t t_main t_a t_c t_b t_d t_g t_w t_n t_got)
    $KL $ULINK -o $T/t/p2.kl $O2 && [ $HAVE_LOADER = 1 ] && [ "$($T/kloader $T/t/p2.kl 2>&1 | grep '^pick=')" = "pick=2" ] && ok "слабый символ не перекрывает сильный при любом порядке объектов" || bad "порядок объектов меняет разрешение слабого символа"
else
    bad "не собрались объекты tests/klink/t_*"
fi
if [ -x $T/kcc0 ]; then   # те же исходники, скомпилированные kcc -c
    mkdir -p $T/tk; fail_k=0
    for f in t_main t_a t_b t_c t_g; do $T/kcc0 -c -Iuserland -Ikcc/include -o $T/tk/$f.o tests/klink/$f.c 2>$T/err || fail_k=1; done
    $T/kcc0 -c -DKL_EXTERN_CM -Iuserland -Ikcc/include -o $T/tk/t_d.o tests/klink/t_d.c 2>$T/err || fail_k=1
    cp $T/t/t_w.o $T/tk/t_w.o; cp $T/t/t_n.o $T/tk/t_n.o; cp $T/t/t_got.o $T/tk/t_got.o
    nasm -f elf64 tests/klink/t_start.asm -o $T/tk/t_start.o
    if [ $fail_k = 0 ]; then
        O=$(objs_of $T/tk t_start t_main t_a t_b t_c t_d t_g t_w t_n t_got)
        ld.lld $ULINK -O0 -o $T/tk/p.lld $O && $KL $ULINK -o $T/tk/p.kl $O && $CMP $T/tk/p.lld $T/tk/p.kl --syms && ok "объекты kcc -c: образ совпал с ld.lld -O0" || bad "объекты kcc -c: образ отличается от ld.lld"
        [ $HAVE_LOADER = 1 ] && { $T/kloader $T/tk/p.kl 2>&1 | head -n -1 | diff -q - tests/klink/t_expected.txt >/dev/null && ok "объекты kcc -c: программа дала ожидаемый вывод" || bad "объекты kcc -c: вывод неверен"; }
    else
        bad "kcc не собрал tests/klink/t_*: $(head -c 200 $T/err)"
    fi
fi

# --- 3. --gc-sections ---
echo "== --gc-sections =="
if build_t $T/tg "-ffunction-sections -fdata-sections"; then
    O=$(objs_of $T/tg $TOBJ)
    ld.lld $ULINK -O0 --gc-sections -o $T/tg/g.lld $O && KLINK_STATS=1 $KL $ULINK --gc-sections -o $T/tg/g.kl $O 2>$T/tg/stat && $KL $ULINK -o $T/tg/n.kl $O \
        && $CMP $T/tg/g.lld $T/tg/g.kl --syms && ok "t_* с -ffunction-sections: --gc-sections совпал с ld.lld ($(sed 's/klink: //' $T/tg/stat | cut -d, -f1,2)); без gc образ $(stat -c%s $T/tg/n.kl), с gc $(stat -c%s $T/tg/g.kl) байт" \
        || bad "t_* --gc-sections отличается от ld.lld"
    nm $T/tg/g.kl | grep -q unused_b && bad "gc оставил недостижимую unused_b" || ok "недостижимые unused_b/unused_b_data удалены, достижимые остались"
    [ $HAVE_LOADER = 1 ] && { $T/kloader $T/tg/g.kl 2>&1 | head -n -1 | diff -q - tests/klink/t_expected.txt >/dev/null && ok "после gc программа выводит то же" || bad "после gc вывод изменился"; }
fi
if ubuild $T/ugc "-ffunction-sections -fdata-sections" 2>$T/err; then
    n=0; nbad=0; removed=0
    while IFS='|' read name objs; do
        ld.lld $ULINK -O0 --gc-sections -o $T/ugc/$name.lld $objs 2>/dev/null
        KLINK_STATS=1 $KL $ULINK --gc-sections -o $T/ugc/$name.kl $objs 2>$T/ugc/$name.st || { bad "gc $name: klink: $(head -c 200 $T/ugc/$name.st)"; nbad=$((nbad+1)); continue; }
        if $CMP $T/ugc/$name.lld $T/ugc/$name.kl --syms >$T/err 2>&1; then n=$((n+1)); removed=$((removed+$(sed 's/.*, \([0-9]*\) removed.*/\1/' $T/ugc/$name.st))); else bad "gc $name отличается от ld.lld: $(head -c 200 $T/err)"; nbad=$((nbad+1)); fi
    done < $T/ugc/progs.txt
    [ $nbad = 0 ] && ok "userland с -ffunction-sections -fdata-sections: $n программ, --gc-sections совпал с ld.lld, удалено $removed секций"
fi

# --- 3b. свой скрипт: PROVIDE, ALIGN у секции, символы границ, KEEP, 1M/4K, несколько PHDRS; другие размеры страницы ---
echo "== другие скрипты и размеры страницы =="
if [ -s $T/t/t_main.o ]; then
    O=$(objs_of $T/t $TOBJ)
    ld.lld -T tests/klink/custom.ld -z max-page-size=4096 -O0 -o $T/t/c.lld $O && $KL -T tests/klink/custom.ld -z max-page-size=4096 -o $T/t/c.kl $O && $CMP $T/t/c.lld $T/t/c.kl --syms \
        && ok "tests/klink/custom.ld (PROVIDE, ALIGN у секции, KEEP, 1M/4K, три PHDRS): совпало с ld.lld -O0, включая символы-границы" || bad "custom.ld: отличие от ld.lld"
    [ $HAVE_LOADER = 1 ] && { $T/kloader $T/t/c.kl 2>&1 | head -n -1 | diff -q - tests/klink/t_expected.txt >/dev/null && ok "custom.ld: программа выводит ожидаемое" || bad "custom.ld: неверный вывод"; }
    for ps in 0x10000 0x200000; do
        ld.lld -T userland/user.ld -z max-page-size=$ps -O0 -o $T/t/q.lld $O && $KL -T userland/user.ld -z max-page-size=$ps -o $T/t/q.kl $O && $CMP $T/t/q.lld $T/t/q.kl && ok "user.ld с -z max-page-size=$ps: совпало с ld.lld" || bad "max-page-size=$ps: отличие от ld.lld"
    done
    $KL $ULINK -e umain -o $T/t/e.kl $O && [ "$(readelf -h $T/t/e.kl | awk '/Entry/{print $4}')" = "$(nm $T/t/e.kl | awk '$3=="umain"{print "0x" $1}' | sed 's/0x0*/0x/')" ] && ok "-e umain меняет точку входа" || bad "-e: точка входа не umain"
fi

# --- 4. ядро ---
echo "== ядро: те же объекты, ld.lld и klink =="
# progs.c включает manifest.h: его создаёт build_clang.sh (`kfstool manifest` по собранным программам) и git его игнорирует. Нет файла в корне - создаём
# тем же инструментом в $T/mf (корень не трогаем) по программам userland, которые только что собрал ld.lld; не получилось - SKIP с причиной, не молчаливый FAIL.
MFI=""; MF_OK=1
if [ ! -f manifest.h ]; then
    mkdir -p $T/mf/bin
    if [ ! -s $T/u/progs.txt ]; then
        MF_OK=0; sk "ядро: сборка и сравнение с ld.lld" "нет manifest.h и нет собранных программ userland для kfstool manifest"
    elif ! $HOSTCC -O1 -DKFS_NO_THREADS -I. -Iarch/x86_64 -o $T/kfstool tools/kfstool.c kfs.c store.c sha256.c 2>$T/err; then
        MF_OK=0; sk "ядро: сборка и сравнение с ld.lld" "нет manifest.h, kfstool не собрался: $(head -c 150 $T/err)"
    else
        while IFS='|' read name objs; do [ -f $T/u/$name.lld ] && cp $T/u/$name.lld $T/mf/bin/$name.elf; done < $T/u/progs.txt
        if $T/kfstool manifest $T/mf/bin/*.elf > $T/mf/manifest.h 2>$T/err; then
            MFI="-I$T/mf"; ok "manifest.h отсутствует в корне: создан kfstool manifest ($(ls $T/mf/bin | wc -l) программ userland) во временном каталоге"
        else
            MF_OK=0; sk "ядро: сборка и сравнение с ld.lld" "нет manifest.h, kfstool manifest не сработал: $(head -c 150 $T/err)"
        fi
    fi
fi
SRCS=$(sed -n '/^C_SOURCES=/p;/^ARCH_SOURCES=/p' build_clang.sh | sed 's/^[A-Z_]*="//;s/"$//' | tr ' ' '\n' | grep -v '^$' | sed 's|\$ARCH|arch/x86_64|')
oname() { echo $1 | tr / _ | sed 's/\.c$/.o/'; }
kobjs() {   # kobjs каталог -> порядок объектов как в build_clang.sh
    local D=$1 o="$1/boot.o"
    for s in $SRCS; do o="$o $D/$(oname $s)"; done
    echo "$o $D/interrupts.o $D/context.o $D/ap_tramp.o $D/uaccess_s.o"
}
build_kernel() {   # build_kernel каталог доп.флаги
    local D=$1 F=$2
    mkdir -p $D
    local CF="-ffreestanding -fno-omit-frame-pointer -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -mno-red-zone -O2 -Wall -Wextra -Werror -mno-mmx -mno-sse -mno-sse2 -I. -Iarch/x86_64 $MFI $F"
    for a in boot interrupts context ap_tramp; do nasm -f elf64 arch/x86_64/$a.s -o $D/$a.o || return 1; done
    nasm -f elf64 arch/x86_64/uaccess.s -o $D/uaccess_s.o || return 1
    for s in $SRCS; do echo "$CLANG $CF -c $s -o $D/$(oname $s)"; done | xargs -P $NPROC -I{} sh -c '{}'
}
boot_check() {   # boot_check ядро метка -> 0, если ядро загрузилось до Mind online; журнал в $T/boot_<метка>.txt
    local K=$1 L=$2
    python3 tools/mkbios.py $T/s1.bin $T/s2.bin $K $T/img_$L.img >/dev/null || return 2
    qemu-system-x86_64 -drive file=$T/img_$L.img,format=raw -m 256 -display none -serial file:$T/boot_$L.txt -no-reboot >/dev/null 2>&1 &
    local q=$! i
    for i in $(seq 1 120); do grep -q "Mind online" $T/boot_$L.txt 2>/dev/null && break; kill -0 $q 2>/dev/null || break; sleep 0.5; done
    kill $q 2>/dev/null; wait $q 2>/dev/null
    tr -d '\r' < $T/boot_$L.txt > $T/boot_$L.clean
    local m r=0
    for m in "Kernel core starting" "4-level paging verified" "Preemptive scheduler online" "Command line ready" "Mind online"; do grep -q "$m" $T/boot_$L.clean || { echo "  нет маркера '$m' ($L)"; r=1; }; done
    grep -qi "kernel panic" $T/boot_$L.clean && { echo "  kernel panic ($L)"; r=1; }
    return $r
}
HAVE_QEMU=1
have qemu-system-x86_64 || { HAVE_QEMU=0; sk "загрузка ядра в QEMU" "нет qemu-system-x86_64"; }
if [ $HAVE_QEMU = 1 ]; then
    nasm -f bin arch/x86_64/bios/stage1.asm -o $T/s1.bin && nasm -f bin arch/x86_64/bios/stage2.asm -o $T/s2.bin || { HAVE_QEMU=0; sk "QEMU" "stage1/stage2 не собрались"; }
fi
if [ $MF_OK = 1 ] && build_kernel $T/k "" 2>$T/err; then
    KO=$(kobjs $T/k)
    ld.lld $KLD $KO -o $T/kira.lld1 && ld.lld -O0 $KLD $KO -o $T/kira.lld0 && $KL $KLD $KO -o $T/kira.kl 2>$T/err || bad "сборка ядра: $(head -c 200 $T/err)"
    if [ -s $T/kira.kl ]; then
        $CMP $T/kira.lld0 $T/kira.kl --syms && ok "ядро (clang): klink совпал с ld.lld -O0 - вход, 4 PT_LOAD/GNU_STACK, .rodata/.text/.data, $(readelf -sW $T/kira.kl | awk 'NR>3' | wc -l) символов" || bad "ядро (clang): klink отличается от ld.lld -O0"
        for s in .rodata .text .data; do objcopy -O binary --only-section=$s $T/kira.kl $T/a.bin; objcopy -O binary --only-section=$s $T/kira.lld0 $T/b.bin; cmp -s $T/a.bin $T/b.bin || bad "ядро: секция $s не побайтно равна"; done
        nm $T/kira.kl | sort > $T/nm.kl; nm $T/kira.lld0 | sort > $T/nm.l0; nm $T/kira.lld1 | sort > $T/nm.l1
        cmp -s $T/nm.kl $T/nm.l0 && ok "nm (адрес, тип, имя): klink и ld.lld -O0 идентичны, $(wc -l < $T/nm.kl) символов" || bad "nm отличается от ld.lld -O0"
        diff <(awk '{print $NF}' $T/nm.kl | sort) <(awk '{print $NF}' $T/nm.l1 | sort) >/dev/null && ok "набор имён символов совпал и с ld.lld по умолчанию (-O1)" || bad "набор имён отличается от ld.lld -O1"
        secsz() { echo $((0x$(readelf -SW $1 | sed 's/\[ *\([0-9]*\)\]/\1/' | awk -v n=$2 '$2==n{print $6}'))); }
        echo "      размеры файла: ld.lld -O1 $(stat -c%s $T/kira.lld1), ld.lld -O0 $(stat -c%s $T/kira.lld0), klink $(stat -c%s $T/kira.kl); .rodata: -O1 $(secsz $T/kira.lld1 .rodata), klink $(secsz $T/kira.kl .rodata) (слияния строк SHF_MERGE у klink нет); .text: -O1 $(secsz $T/kira.lld1 .text), klink $(secsz $T/kira.kl .text); .data: -O1 $(secsz $T/kira.lld1 .data), klink $(secsz $T/kira.kl .data)"
        if [ $HAVE_QEMU = 1 ]; then
            boot_check $T/kira.lld1 lld && ok "QEMU: ядро ld.lld загрузилось (Kernel core starting ... Mind online)" || bad "QEMU: ядро ld.lld не загрузилось"
            boot_check $T/kira.kl klink && ok "QEMU: ядро klink загрузилось (Kernel core starting ... Mind online)" || bad "QEMU: ядро klink не загрузилось"
            # boot_check обрывает QEMU сразу после "Mind online": хвост журнала зависит от скорости хоста, поэтому сравниваются строки до этого маркера включительно
            bootlog() { sed '/Mind online/q' $1 | sed 's/[0-9]\+/N/g' | grep -v 'Self baseline'; }
            diff <(bootlog $T/boot_lld.clean) <(bootlog $T/boot_klink.clean) >/dev/null \
                && ok "журналы загрузки ld.lld и klink совпали построчно до Mind online (числа и хеш образа нормализованы)" || bad "журналы загрузки отличаются: $(diff <(bootlog $T/boot_lld.clean) <(bootlog $T/boot_klink.clean) | head -4)"
            tests/bios_boot_test.sh $T/kira.kl >$T/bios.log 2>&1 && ok "tests/bios_boot_test.sh с ядром klink: загрузка, отказ на CRC, на p_paddr, на малой памяти" || bad "tests/bios_boot_test.sh с ядром klink: $(grep FAIL $T/bios.log | head -3)"
        fi
    fi
elif [ $MF_OK = 1 ]; then
    bad "ядро не собралось clang: $(head -c 300 $T/err)"
fi
# ядро, собранное kcc (KIRA_CC=kcc)
if [ -x $T/kcc0 ] && [ $MF_OK = 1 ]; then
    mkdir -p $T/kk; kfail=0
    for s in $SRCS; do echo "$T/kcc0 -c -I. -Iarch/x86_64 $MFI -Ikcc/include -o $T/kk/$(oname $s) $s"; done | xargs -P $NPROC -I{} sh -c '{}' 2>$T/err || kfail=1
    for a in boot interrupts context ap_tramp; do cp $T/k/$a.o $T/kk/; done; cp $T/k/uaccess_s.o $T/kk/
    if [ $kfail = 0 ]; then
        KO=$(kobjs $T/kk)
        ld.lld -O0 $KLD $KO -o $T/kk.lld && $KL $KLD $KO -o $T/kk.kl 2>$T/err && $CMP $T/kk.lld $T/kk.kl --syms && ok "ядро из объектов kcc -c: klink совпал с ld.lld -O0 ($(stat -c%s $T/kk.kl) байт)" || bad "ядро из объектов kcc: отличие или ошибка $(head -c 200 $T/err)"
        if [ $HAVE_QEMU = 1 ] && [ -s $T/kk.kl ]; then boot_check $T/kk.kl kcc && ok "QEMU: ядро (kcc + klink) загрузилось до Mind online" || bad "QEMU: ядро (kcc + klink) не загрузилось"; fi
    else
        bad "kcc не собрал ядро: $(head -c 200 $T/err)"
    fi
fi
# ядро с -ffunction-sections и --gc-sections
if [ "${KLINK_QUICK:-0}" != 1 ]; then
    if [ $MF_OK = 1 ] && build_kernel $T/kg "-ffunction-sections -fdata-sections" 2>$T/err; then
        KO=$(kobjs $T/kg)
        ld.lld -O0 --gc-sections $KLD $KO -o $T/kg.lld && KLINK_STATS=1 $KL --gc-sections $KLD $KO -o $T/kg.kl 2>$T/kg.st && $CMP $T/kg.lld $T/kg.kl --syms \
            && ok "ядро -ffunction-sections --gc-sections: совпало с ld.lld ($(sed 's/klink: //' $T/kg.st | cut -d, -f1,2), $(stat -c%s $T/kg.kl) байт)" || bad "ядро с gc отличается от ld.lld"
        if [ $HAVE_QEMU = 1 ] && [ -s $T/kg.kl ]; then boot_check $T/kg.kl kgc && ok "QEMU: ядро (--gc-sections, klink) загрузилось до Mind online" || bad "QEMU: ядро с gc не загрузилось"; fi
    elif [ $MF_OK = 1 ]; then
        bad "ядро с -ffunction-sections не собралось"
    fi
else
    sk "ядро с --gc-sections" "KLINK_QUICK=1"
fi

# --- 5. негативные тесты ---
echo "== негативные тесты =="
N=$T/n; mkdir -p $N
neg() {   # neg имя регэксп команда... : код выхода 1, сообщение подходит, выходной файл не создан
    local name=$1 pat=$2; shift 2
    rm -f $N/out
    "$@" >$N/o.txt 2>$N/e.txt; local rc=$?
    if { [ $rc = 1 ] || [ $rc = 2 ]; } && grep -q -- "$pat" $N/e.txt && [ ! -e $N/out ]; then ok "отказ: $name -> $(head -c 110 $N/e.txt | head -1)"; else bad "$name: код $rc, сообщение '$(head -c 160 $N/e.txt)' (ожидалось /$pat/)"; fi
}
mk_c() { echo "$2" > $N/$1.c; $CLANG -ffreestanding -fno-pic -fno-pie -O1 -mno-red-zone -mno-sse -c $N/$1.c -o $N/$1.o ${3:-}; }
mk_c undef 'int nothere(void); int _start(void) { return nothere(); }'
mk_c dup1 'int dup(void) { return 1; } int _start(void) { return dup(); }'
mk_c dup2 'int dup(void) { return 2; }'
mk_c tls '__thread int t; int _start(void) { return t; }'
printf 'global ext_sym\nsection .data\next_sym: dd 1\n' > $N/ext.asm; nasm -f elf64 -o $N/ext.o $N/ext.asm
printf 'default rel\nglobal _start\nextern ext_sym\nsection .text\n_start:\n  mov rax, [rel ext_sym wrt ..got]\n  ret\n' > $N/gotreloc.asm; nasm -f elf64 -o $N/gotreloc.o $N/gotreloc.asm
printf 'global _start\nextern ext_sym\nsection .text\n_start:\n  mov eax, ext_sym\n  ret\n' > $N/abs32.asm; nasm -f elf64 -o $N/abs32.o $N/abs32.asm
printf 'default rel\nglobal _start\nextern ext_sym\nsection .text\n_start:\n  mov rax, [rel ext_sym wrt ..gottpoff]\n  ret\n' > $N/gotpc.asm; nasm -f elf64 -o $N/gotpc.o $N/gotpc.asm
printf 'default rel\nglobal _start\nextern ext_sym\nsection .text\n_start:\n  mov eax, [ext_sym]\n  ret\n' > $N/pc32.asm; nasm -f elf64 -o $N/pc32.o $N/pc32.asm
printf 'global _start\nsection .text\n_start: ret\nsection .weirdalloc alloc write\ndd 5\n' > $N/orphan.asm; nasm -f elf64 -o $N/orphan.o $N/orphan.asm 2>/dev/null
printf 'global _start\nsection .text\n_start: lea rax, [rel hidden_data]\n ret\nsection .rodata\nhidden_data: dd 5\n' > $N/disc.asm; nasm -f elf64 -o $N/disc.o $N/disc.asm
K="$KL $ULINK -o $N/out"
neg "неопределённый символ" "undefined reference to 'nothere'" $K $N/undef.o
neg "дубликат символа" "duplicate symbol 'dup'" $K $N/dup1.o $N/dup2.o
neg "TLS" "thread-local" $K $N/tls.o
# GOT: нерелаксируемая ссылка (nasm: R_X86_64_GOTPCREL) получает ячейку в .got с абсолютным адресом символа; образ совпадает с ld.lld
printf 'default rel\nglobal _start\nextern other_fn\nsection .text\n_start:\n  mov rax, [rel other_fn wrt ..got]\n  ret\n' > $N/gotself.asm; nasm -f elf64 -o $N/gotself.o $N/gotself.asm
printf 'global other_fn\nsection .text\nother_fn: ret\n' > $N/gotfn.asm; nasm -f elf64 -o $N/gotfn.o $N/gotfn.asm
printf 'global _start\nextern ext_sym\nsection .text\n_start:\n  mov rax, [ext_sym wrt ..got]\n  ret\n' > $N/got32.asm; nasm -f elf64 -o $N/got32.o $N/got32.asm
if $KL $ULINK -o $N/gl.kl $N/gotreloc.o $N/ext.o 2>$N/e.txt && ld.lld $ULINK -O0 -o $N/gl.lld $N/gotreloc.o $N/ext.o && $CMP $N/gl.lld $N/gl.kl --syms >$N/e.txt 2>&1 \
    && objcopy -O binary --only-section=.got $N/gl.kl $N/gl.got \
    && [ "$(od -An -tx8 -v $N/gl.got | tr -d ' \n')" = "$(nm $N/gl.kl | awk '$3=="ext_sym"{print $1}')" ]; then
    ok "GOT: R_X86_64_GOTPCREL без релаксации -> ячейка .got с адресом ext_sym, образ совпал с ld.lld"
else
    bad "GOT: нерелаксируемый GOTPCREL: $(head -c 200 $N/e.txt)"
fi
neg "R_X86_64_GOT32 (индекс в GOT, нет поддержки)" "unsupported relocation type 3 (R_X86_64_GOT32)" $K $N/got32.o $N/ext.o
printf 'ENTRY(_start)\nPHDRS { t PT_LOAD FLAGS(5); }\nSECTIONS { . = 0x1000; .text : { *(.text*) } :t }\n' > $N/gotnw.ld
neg "GOT нужна, а в скрипте нет записываемой секции с данными" "no writable output section" $KL -T $N/gotnw.ld -o $N/out $N/gotself.o $N/gotfn.o
printf 'ENTRY(_start)\nSECTIONS { . = 0x1000; .text : { *(.text*) } }\n' > $N/gotauto.ld
if $KL -T $N/gotauto.ld -z max-page-size=4096 -o $N/ga.kl $N/gotself.o $N/gotfn.o 2>$N/e.txt && [ "$(readelf -lW $N/ga.kl | grep -c LOAD)" = 2 ] && [ "$(readelf -SW $N/ga.kl | sed 's/\[ *\([0-9]*\)\]/\1/' | awk '$2==".got"{print $4}')" = 0000000000002000 ]; then
    ok "GOT без PHDRS и без секции данных: .got в отдельном RW PT_LOAD на своей странице"
else
    bad "GOT без PHDRS: $(head -c 200 $N/e.txt)"
fi
neg "неподдержанное перемещение R_X86_64_GOTTPOFF" "unsupported relocation type 22 (R_X86_64_GOTTPOFF) against 'ext_sym'" $K $N/gotpc.o $N/ext.o
neg "переполнение R_X86_64_32 (адрес 0x8000000000)" "R_X86_64_32 overflows against 'ext_sym'" $K $N/abs32.o $N/ext.o
printf 'ENTRY(_start)\nSECTIONS { . = 0x1000; .text : { *(.text*) } . = 0x20000000000; .data : { *(.data*) } }\n' > $N/far.ld
neg "переполнение R_X86_64_PC32 (секции дальше 2 ГиБ)" "PC32 overflows against 'ext_sym'" $KL -T $N/far.ld -o $N/out $N/pc32.o $N/ext.o
neg "секция не покрыта скриптом" "not placed by the linker script" $K $N/orphan.o
printf 'ENTRY(_start)\nSECTIONS { . = 0x8000000000; .text : { *(.text*) } /DISCARD/ : { *(.rodata*) } }\n' > $N/disc.ld
neg "ссылка в отброшенную секцию" "discarded section" $KL -T $N/disc.ld -o $N/out $N/disc.o
printf 'ENTRY(nosuch)\nSECTIONS { . = 0x8000000000; .text : { *(.text*) } /DISCARD/ : { *(.rodata*) *(.data*) } }\n' > $N/noentry.ld
neg "нет символа входа" "entry symbol 'nosuch' is not defined" $KL -T $N/noentry.ld -o $N/out $N/disc.o
for sc in 'MEMORY { r : ORIGIN = 0, LENGTH = 1M }' 'SECTIONS { . = 0x1000; .text : { *(SORT(.text*)) } }' 'SECTIONS { . = SIZEOF(.x); }' 'SECTIONS { /* ' 'SECTIONS { .text : { *(.text*) } }' 'OUTPUT_FORMAT(elf64-x86-64)' 'SECTIONS { . = 0x1000; .text : AT(0x2000) { *(.text*) } }' 'SECTIONS { . = 0x1000; .text : { *(.text*) } } PHDRS { x PT_NOTE; }' 'SECTIONS { . = ((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((((1)))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))))); }'; do
    echo "$sc" > $N/bad.ld
    neg "скрипт: $(echo "$sc" | cut -c1-40)" "script\|SECTIONS\|unsupported" $KL -T $N/bad.ld -o $N/out $N/disc.o
done
head -c 100 $N/undef.o > $N/trunc.o; neg "обрезанный .o" "outside the file\|not an ELF\|bad" $K $N/trunc.o
printf 'not an object file at all, just text...........................................................' > $N/text.o; neg "не ELF" "not an ELF" $K $N/text.o
python3 - $N <<'PY'
import sys
n = sys.argv[1]
d = bytearray(open(n + '/undef.o', 'rb').read())
m = bytearray(d); m[18] = 0xb7; m[19] = 0; open(n + '/aarch64.o', 'wb').write(m)          # e_machine = EM_AARCH64
m = bytearray(d); m[16] = 2; open(n + '/etexec.o', 'wb').write(m)                         # e_type = ET_EXEC
m = bytearray(d); m[60] = 0xff; m[61] = 0xff; open(n + '/shnum.o', 'wb').write(m)         # e_shnum огромен
m = bytearray(d); m[4] = 1; open(n + '/elf32.o', 'wb').write(m)                           # ELFCLASS32
m = bytearray(d)
shoff = int.from_bytes(m[40:48], 'little')
for i in range(1, 6):                                                                     # sh_offset секций за пределами файла
    o = shoff + 64 * i + 24
    m[o:o + 8] = (1 << 40).to_bytes(8, 'little')
open(n + '/secoff.o', 'wb').write(m)
open(n + '/ar.a', 'wb').write(b'!<arch>\n' + b'x' * 64)
PY
neg "e_machine не x86-64" "not an x86-64 object" $K $N/aarch64.o
neg "ET_EXEC на входе" "not a relocatable object" $K $N/etexec.o
neg "ELF32" "little-endian ELF64" $K $N/elf32.o
neg "e_shnum огромен" "section count\|outside the file" $K $N/shnum.o
neg "смещения секций за файлом" "outside the file\|bad" $K $N/secoff.o
neg "архив .a" "archives" $K $N/ar.a
neg "нет входных файлов" "usage" $KL -T userland/user.ld -o $N/out
neg "неизвестная опция" "unsupported option" $KL $ULINK --relax -o $N/out $N/undef.o
neg "неподдержанный -z" "unsupported option -z" $KL -T userland/user.ld -z execstack -o $N/out $N/undef.o
neg "нет файла" "cannot read" $K $N/nonexistent.o
else
    sk "userland/ядро/негативные тесты" "нет clang, ld.lld, nasm или python3"
fi

# --- 6. фаззинг ---
echo "== фаззинг (ASan+UBSan) =="
if [ $HAVE_TOOLCHAIN = 1 ]; then
    if $HOSTCC -std=c99 -D_DEFAULT_SOURCE -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $T/fuzz tests/klink/fuzz.c klink/klink.c 2>$T/err; then
        FO=$(objs_of $T/t $TOBJ)
        ITER=${KLINK_FUZZ_ITER:-6000}
        for seed in 1 2 3; do
            if $T/fuzz $ITER $seed userland/user.ld $FO >$T/fz.txt 2>&1; then ok "fuzz seed $seed: $(cat $T/fz.txt)"; else bad "fuzz seed $seed: $(tail -5 $T/fz.txt)"; fi
        done
        FK=$(objs_of $T/tk t_start t_main t_a t_b t_c t_d t_g t_w t_n t_got)
        if $T/fuzz $ITER 4 userland/user.ld $FK >$T/fz.txt 2>&1; then ok "fuzz на объектах kcc -c: $(cat $T/fz.txt)"; else bad "fuzz (kcc): $(tail -5 $T/fz.txt)"; fi
        # скрипты ядра и userland, настоящий объект ядра
        if $T/fuzz $ITER 5 arch/x86_64/linker.ld $T/k/boot.o $T/k/interrupts.o $T/k/context.o $T/k/ap_tramp.o $T/k/uaccess_s.o >$T/fz.txt 2>&1; then ok "fuzz на nasm-объектах ядра: $(cat $T/fz.txt)"; else bad "fuzz (nasm): $(tail -5 $T/fz.txt)"; fi
    else
        bad "fuzz не собрался: $(head -c 200 $T/err)"
    fi
else
    sk "фаззинг" "нет инструментов для объектов"
fi

echo "итого: $pass ok, $fail FAIL, $skip SKIP"
[ $fail = 0 ] && echo "klink: PASS" || { echo "klink: FAIL"; exit 1; }
