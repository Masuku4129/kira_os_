#!/bin/bash
# L3: один дефект - один фрагмент на C (kcc, clang, gcc), C++ (clang++, g++) и Rust. Что произошло при запуске (классификация по факту: SILENT / ABORT / SIGNAL / SANITIZER / PANIC / COMPILE_ERROR).
. "$(dirname "$0")/lang_common.sh"
S=$RD/scripts/lang_src/safety; D=$WORK/l3; mkdir -p $D; LOG=$LOGD/lang_3_safety_raw.txt; : > $LOG
build_kcc
classify() { # rc out
  local rc=$1 out=$2
  if grep -qE "ERROR: (Address|Thread)Sanitizer|WARNING: ThreadSanitizer|AddressSanitizer" <<<"$out"; then echo SANITIZER
  elif grep -q "runtime error" <<<"$out"; then echo SANITIZER_UBSAN
  elif grep -q "panicked at" <<<"$out"; then echo PANIC
  elif [ $rc -ge 128 ]; then echo "SIGNAL$((rc-128))"
  elif [ $rc -ne 0 ]; then echo "EXIT$rc"
  else echo SILENT; fi; }
# run1 DEFECT CAND EXE [args]
run1() {
  local def=$1 cand=$2 exe=$3; shift 3
  local out rc; local ts="taskset -c 0"; [ $def = race ] && ts=""; out=$(cd $D && $ts timeout 20 $exe "$@" 2>&1); rc=$?
  printf '=== %s | %s | args=[%s] | rc=%s\n%s\n' $def "$cand" "$*" $rc "$(echo "$out" | head -12)" >> $LOG
  local cls; cls=$(classify $rc "$out"); local one; one=$(echo "$out" | grep -vE "^\s*$" | head -3 | tr '\n"' '| ' | cut -c1-170)
  R L3 x86_64 $ENV "$cand" $def outcome 1 $cls class $rc "args=[$*] $one"
}
cfail() { if grep -qE "libclang_rt|ABI mismatch" $D/cerr; then R L3 x86_64 $ENV "$1" $2 outcome 1 NOT_RUN class NOTRUN "NOT RUN: нет рантайма санитайзера/пересборки std: $(grep -E 'libclang_rt|ABI mismatch' $D/cerr | head -1 | cut -c1-140 | tr '"' ' ')"; return; fi
  R L3 x86_64 $ENV "$1" $2 outcome 1 COMPILE_ERROR class 1 "$(head -c 400 $D/cerr | tr '\n"' '| ')"; printf '=== %s | %s | COMPILE_ERROR\n%s\n' $2 "$1" "$(head -c 600 $D/cerr)" >> $LOG; }
for def in stack heap uaf ovf race; do
  src=$S/c_$def.c; PT=""; [ $def = race ] && PT="-pthread"
  # kcc: объект + нативная линковка
  if $KCC -c -o $D/$def.kcc.o $src 2>$D/cerr && gcc -no-pie $PT $D/$def.kcc.o -o $D/$def.kcc 2>>$D/cerr; then run1 $def C-kcc $D/$def.kcc; else cfail C-kcc $def; fi
  for cc in clang gcc; do for o in O0 O2; do
    if $cc -$o -w $PT $src -o $D/$def.$cc.$o 2>$D/cerr; then run1 $def C-$cc-$o $D/$def.$cc.$o; else cfail C-$cc-$o $def; fi
  done; done
  for cc in clang gcc; do
    san="address,undefined"; [ $def = race ] && san="thread"
    if $cc -O1 -g -w -fsanitize=$san $PT $src -o $D/$def.$cc.san 2>$D/cerr; then run1 $def C-$cc-san-${san//,/+} $D/$def.$cc.san; else cfail C-$cc-san-${san//,/+} $def; fi
  done
  # C++
  src=$S/cpp_$def.cpp
  for cc in clang++ g++; do for o in O0 O2; do
    if $cc -$o -w $PT $src -o $D/$def.$cc.$o 2>$D/cerr; then
      run1 $def Cpp-$cc-$o $D/$def.$cc.$o; { [ $def = stack ] || [ $def = heap ]; } && run1 $def Cpp-$cc-$o-at $D/$def.$cc.$o at
    else cfail Cpp-$cc-$o $def; fi
  done; done
  for cc in clang++ g++; do
    san="address,undefined"; [ $def = race ] && san="thread"
    if $cc -O1 -g -w -fsanitize=$san $PT $src -o $D/$def.$cc.san 2>$D/cerr; then run1 $def Cpp-$cc-san-${san//,/+} $D/$def.$cc.san; else cfail Cpp-$cc-san-${san//,/+} $def; fi
  done
done
# Rust
for def in stack heap ovf uaf race; do
  case $def in uaf|race) variants="$def ${def}_safe";; *) variants=$def;; esac
  for v in $variants; do [ -f $S/rs_$v.rs ] || continue
    for mode in debug release; do
      fl=""; [ $mode = release ] && fl="-C opt-level=3"
      if rustc --edition 2021 $fl $S/rs_$v.rs -o $D/rs_$v.$mode 2>$D/cerr; then
        run1 $def Rust-$mode$( [[ $v = *_safe ]] && echo -safe ) $D/rs_$v.$mode
        { [ $def = stack ] || [ $def = heap ]; } && run1 $def Rust-$mode-unchecked $D/rs_$v.$mode x
      else cfail Rust-$mode$( [[ $v = *_safe ]] && echo -safe ) $def; fi
    done
  done
done
# Rust + санитайзеры (нестабильный флаг через RUSTC_BOOTSTRAP=1; если не выйдет - COMPILE_ERROR с причиной)
for def in stack heap uaf; do
  if RUSTC_BOOTSTRAP=1 rustc --edition 2021 -Zsanitizer=address -C opt-level=1 -C debuginfo=1 $S/rs_$def.rs -o $D/rs_$def.asan 2>$D/cerr; then
    run1 $def Rust-asan $D/rs_$def.asan; { [ $def = stack ] || [ $def = heap ]; } && run1 $def Rust-asan-unchecked $D/rs_$def.asan x
  else cfail Rust-asan $def; fi
done
if RUSTC_BOOTSTRAP=1 rustc --edition 2021 -Zsanitizer=thread -C opt-level=1 $S/rs_race.rs -o $D/rs_race.tsan 2>$D/cerr; then run1 race Rust-tsan $D/rs_race.tsan; else cfail Rust-tsan race; fi
which cargo-miri >/dev/null 2>&1 || R L3 x86_64 $ENV Rust-miri all outcome 1 NOT_RUN class NOTRUN "NOT RUN: Miri не установлен"
echo L3 done
