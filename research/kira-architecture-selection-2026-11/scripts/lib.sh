# общие функции: rec EXP ARCH ENV CAND WORKLOAD METRIC REP VALUE UNIT EXIT NOTE >> файл
rec() { printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' "$@"; }
# t_ms CMD...: wall-time в мс (минимум разрешения 0.1) на ядре 1; печатает значение
t_ms() { local s e; s=$(date +%s%N); taskset -c 1 "$@" >/dev/null 2>&1; local rc=$?; e=$(date +%s%N); echo "$(( (e-s)/1000 )) $rc"; }
