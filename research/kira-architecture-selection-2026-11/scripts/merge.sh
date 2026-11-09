#!/bin/sh
cd "$(dirname "$0")/../data" || exit 1
{ echo "exp,arch,env,candidate,workload,metric,rep,value,unit,exit_code,note"; for f in results_*.csv; do tail -n +2 "$f"; done; } > results.csv
wc -l results.csv
