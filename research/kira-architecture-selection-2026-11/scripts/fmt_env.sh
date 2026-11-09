# общая среда секции «форматы» (source). Все процессы - на ядре 0 (ядро 1 занято скоростными замерами другого помощника).
FMT_RESEARCH=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
FMT_REPO=$(cd "$FMT_RESEARCH/../.." && pwd)
FMT_ART=${FMT_ART:-/home/claude/kira_os_}            # готовые артефакты сборки (только чтение)
FMT_DATA=$FMT_RESEARCH/data; FMT_LOGS=$FMT_RESEARCH/logs; FMT_CSV=$FMT_DATA/results_fmt.csv
. "$FMT_RESEARCH/scripts/lib.sh"
C0="taskset -c 0"
[ -f "$FMT_CSV" ] || echo "exp,arch,env,candidate,workload,metric,rep,value,unit,exit_code,note" > "$FMT_CSV"
# frec ARCH CAND WORKLOAD METRIC REP VALUE UNIT EXIT NOTE  -> строка в results_fmt.csv (env=vm-2vcpu-core0)
frec() { rec "${FMT_EXP:-fmt}" "$1" "vm-core0" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" >> "$FMT_CSV"; }
