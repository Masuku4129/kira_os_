#!/bin/bash
# L5: воспроизводимость. Объект/исполняемый файл собирается дважды (разные каталоги, пауза 2 с между сборками), сравнивается sha256.
# Проверяем: чистая сборка; с отладочной информацией (пути в -g) без и с remap; __DATE__/__TIME__; SOURCE_DATE_EPOCH; rustc (--remap-path-prefix, codegen-units).
. "$(dirname "$0")/lang_common.sh"; build_kcc
S=$RD/scripts/lang_src; D=$WORK/l5; rm -rf $D; mkdir -p $D/a $D/b
cp $BENCH/sha256.c $BENCH/sort.c $D/a/; cp $BENCH/sha256.c $BENCH/sort.c $D/b/
cat > $D/date.c <<'EOT'
const char *stamp(void) { return __DATE__ " " __TIME__; }
EOT
cp $D/date.c $D/a/; cp $D/date.c $D/b/
cp $S/bench4.rs $D/a/; cp $S/bench4.rs $D/b/
h() { sha256sum "$1" | cut -c1-16; }
cmpb() { # cmpb CAND WL FLAGDESC HASHA HASHB
  local eq=0; [ "$4" = "$5" ] && eq=1; R L5 x86_64 $ENV "$1" $2 hash_equal 1 $eq bool $((1-eq)) "$3: $4 / $5"; }
SEC() { sleep 2; }
for w in sha256 sort; do
  for c in "kcc|$KCC -c" "clang-O2|clang -O2 -c" "gcc-O2|gcc -O2 -c"; do
    n=${c%%|*}; cmd=${c#*|}
    (cd $D/a && $cmd $w.c -o $w.$n.o); SEC; (cd $D/b && $cmd $w.c -o $w.$n.o)
    cmpb $n $w "чистая сборка в разных каталогах, +2 с" $(h $D/a/$w.$n.o) $(h $D/b/$w.$n.o)
  done
  # с -g: пути в отладочной информации
  for c in "clang-O2-g|clang -O2 -g -c" "gcc-O2-g|gcc -O2 -g -c"; do n=${c%%|*}; cmd=${c#*|}
    (cd $D/a && $cmd $D/a/$w.c -o $w.$n.o); (cd $D/b && $cmd $D/b/$w.c -o $w.$n.o)
    cmpb $n $w "-g, абсолютные пути каталогов a и b различны" $(h $D/a/$w.$n.o) $(h $D/b/$w.$n.o)
  done
  (cd $D/a && clang -O2 -g -fdebug-prefix-map=$D/a=. -c $D/a/$w.c -o $w.cr.o); (cd $D/b && clang -O2 -g -fdebug-prefix-map=$D/b=. -c $D/b/$w.c -o $w.cr.o)
  cmpb clang-O2-g-remap $w "-g -fdebug-prefix-map" $(h $D/a/$w.cr.o) $(h $D/b/$w.cr.o)
  (cd $D/a && gcc -O2 -g -ffile-prefix-map=$D/a=. -c $D/a/$w.c -o $w.gr.o); (cd $D/b && gcc -O2 -g -ffile-prefix-map=$D/b=. -c $D/b/$w.c -o $w.gr.o)
  cmpb gcc-O2-g-remap $w "-g -ffile-prefix-map" $(h $D/a/$w.gr.o) $(h $D/b/$w.gr.o)
done
# исполняемые файлы (линковка): sha256, одинаковые объекты
for ld in lld bfd; do clang -fuse-ld=$ld -no-pie -O2 $D/a/sha256.c -o $D/a/x.$ld -Dmain=main 2>/dev/null; SEC; clang -fuse-ld=$ld -no-pie -O2 $D/a/sha256.c -o $D/b/x.$ld -Dmain=main 2>/dev/null; cmpb link-$ld sha256 "линковка: clang+ld.$ld, повторная линковка, +2 с (build-id включён)" $(h $D/a/x.$ld) $(h $D/b/x.$ld); done
# __DATE__/__TIME__
for c in "kcc|$KCC -c" "clang-O2|clang -O2 -c" "gcc-O2|gcc -O2 -c"; do n=${c%%|*}; cmd=${c#*|}
  (cd $D/a && $cmd date.c -o date.$n.o 2>$D/err) ; rc1=$?; SEC; (cd $D/b && $cmd date.c -o date.$n.o 2>>$D/err)
  if [ -s $D/a/date.$n.o ]; then cmpb $n date-macro "__DATE__ __TIME__, сборки с интервалом 2 с" $(h $D/a/date.$n.o) $(h $D/b/date.$n.o); else R L5 x86_64 $ENV $n date-macro hash_equal 1 "" bool 1 "kcc не собрал date.c: $(head -c 100 $D/err | tr '\n"' '  ')"; fi
done
# фиксация времени: SOURCE_DATE_EPOCH (gcc) и -Wdate-time/-D переопределение
(cd $D/a && SOURCE_DATE_EPOCH=1700000000 gcc -O2 -c date.c -o date.gse.o); SEC; (cd $D/b && SOURCE_DATE_EPOCH=1700000000 gcc -O2 -c date.c -o date.gse.o)
cmpb gcc-O2-SDE date-macro "SOURCE_DATE_EPOCH=1700000000 у gcc" $(h $D/a/date.gse.o) $(h $D/b/date.gse.o)
(cd $D/a && SOURCE_DATE_EPOCH=1700000000 clang -O2 -c date.c -o date.cse.o); SEC; (cd $D/b && SOURCE_DATE_EPOCH=1700000000 clang -O2 -c date.c -o date.cse.o)
cmpb clang-O2-SDE date-macro "SOURCE_DATE_EPOCH=1700000000 у clang" $(h $D/a/date.cse.o) $(h $D/b/date.cse.o)
# rustc
RF="--crate-type=lib --emit=obj -C opt-level=3 -C panic=abort --cfg w_sha256"
(cd $D/a && rustc $RF bench4.rs -o rs.o); SEC; (cd $D/b && rustc $RF bench4.rs -o rs.o)
cmpb rustc-O3 sha256 "чистая сборка в разных каталогах, +2 с (без -g)" $(h $D/a/rs.o) $(h $D/b/rs.o)
(cd $D/a && rustc $RF -g $D/a/bench4.rs -o rs_g.o); (cd $D/b && rustc $RF -g $D/b/bench4.rs -o rs_g.o)
cmpb rustc-O3-g sha256 "-g, абсолютные пути различны" $(h $D/a/rs_g.o) $(h $D/b/rs_g.o)
(cd $D/a && rustc $RF -g --remap-path-prefix=$D/a=. $D/a/bench4.rs -o rs_gr.o); (cd $D/b && rustc $RF -g --remap-path-prefix=$D/b=. $D/b/bench4.rs -o rs_gr.o)
cmpb rustc-O3-g-remap sha256 "-g --remap-path-prefix" $(h $D/a/rs_gr.o) $(h $D/b/rs_gr.o)
(cd $D/a && rustc $RF -C codegen-units=16 bench4.rs -o rs16.o); (cd $D/b && rustc $RF -C codegen-units=16 bench4.rs -o rs16.o)
cmpb rustc-O3-cgu16 sha256 "codegen-units=16 (по умолчанию для не-инкрементальной release в crate) повторная сборка" $(h $D/a/rs16.o) $(h $D/b/rs16.o)
# ассемблеры и линкеры на одном входе
for n in nasm; do (cd $D/a && nasm -f elf64 $REPO/arch/x86_64/uaccess.s -o as_$n.o); SEC; (cd $D/b && nasm -f elf64 $REPO/arch/x86_64/uaccess.s -o as_$n.o); cmpb $n uaccess.s "ассемблер, +2 с" $(h $D/a/as_$n.o) $(h $D/b/as_$n.o); done
[ -x $WORK/kas ] || taskset -c 0 gcc -std=c99 -O2 -w -o $WORK/kas $REPO/kas/kas.c $REPO/kas/kas_main.c
(cd $D/a && $WORK/kas -f elf64 -o as_kas.o $REPO/arch/x86_64/uaccess.s); SEC; (cd $D/b && $WORK/kas -f elf64 -o as_kas.o $REPO/arch/x86_64/uaccess.s); cmpb kas uaccess.s "ассемблер, +2 с" $(h $D/a/as_kas.o) $(h $D/b/as_kas.o)
echo L5 done
