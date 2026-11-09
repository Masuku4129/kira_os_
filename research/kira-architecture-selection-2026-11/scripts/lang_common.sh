# общие функции раздела "языки/тулчейны" (CG-20261109-013). Подключается: . lang_common.sh
# Правила: скорость - ядро 1 (taskset -c 1), сборки - ядро 0 (taskset -c 0); >=9 повторов.
RD=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)          # research/kira-architecture-selection-2026-11
REPO=$(cd "$RD/../.." && pwd)
. "$RD/scripts/lib.sh"
CSV=${CSV:-$RD/data/results_lang.csv}
LOGD=$RD/logs
WORK=${WORK:-/tmp/claude-0/-home-claude/a2574043-553e-5ae0-b042-b737ad0998e8/scratchpad/work}
mkdir -p "$WORK" "$LOGD"
REPS=${REPS:-11}; BREPS=${BREPS:-9}
[ -s "$CSV" ] || echo "exp,arch,env,candidate,workload,metric,rep,value,unit,exit_code,note" > "$CSV"
ENV=vm-x86_64-2vcpu
R() { rec "$@" >> "$CSV"; }   # R EXP ARCH ENV CAND WL METRIC REP VAL UNIT EXIT NOTE
nowus() { echo $(( $(date +%s%N) / 1000 )); }
# run_cpu CMD... : время мкс на ядре 1, печатает "us rc"
run_cpu() { local s e rc; s=$(date +%s%N); taskset -c 1 "$@" >/dev/null 2>&1; rc=$?; e=$(date +%s%N); echo "$(( (e-s)/1000 )) $rc"; }
# build_time CMD... : время мкс на ядре 0 + пик RSS (КБ) через maxrss (wait4/ru_maxrss; GNU time на машине нет); печатает "us kb rc"
build_time() { local f=$WORK/.tv s e rc; s=$(date +%s%N); taskset -c 0 $WORK/maxrss $f "$@" >/dev/null 2>$WORK/.berr; rc=$?; e=$(date +%s%N)
  echo "$(( (e-s)/1000 )) $(awk '/Maximum resident/{print $NF}' $f) $rc"; }
# text_size OBJ : сумма размеров секций .text* (байты) из ET_REL/ET_EXEC
text_size() { size -A "$1" 2>/dev/null | awk '$1 ~ /^\.text/ {s+=$2} END{print s+0}'; }
stats() { # stats v1 v2 ... -> "median min max"
  printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{m=(NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2; printf "%d %d %d", m, a[1], a[NR]}'; }
KCC=$WORK/kcc; KLOADER=$WORK/kloader
build_kcc() { [ -x $KCC ] || taskset -c 0 clang -std=c99 -O1 -w -o $KCC $REPO/kcc/*.c; [ -x $KLOADER ] || taskset -c 0 clang -std=gnu99 -O1 -w -DKIRA_HOST_TEST -o $KLOADER $REPO/tests/kcc/kloader.c $REPO/elf.c; }
[ -x $WORK/maxrss ] || clang -O1 -o $WORK/maxrss $RD/scripts/lang_src/maxrss.c
BENCH=$REPO/tests/kcc/bench
