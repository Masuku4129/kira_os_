#!/bin/bash
# Итог стенда коротко: ждёт прогон коммита (по умолчанию HEAD), печатает только сбои и строки по шаблону.
#   ci/brief.sh [коммит] [шаблон-grep для журналов] [сценарий...]
#   пример: ci/brief.sh HEAD 'HDA|звук' vm-boot-virtio arm64/arm64-gicv3
cd "$(dirname "$0")/.."
br=$(git rev-parse --abbrev-ref HEAD); case "$br" in main|stable) cb=ci-results;; *) cb="ci-results-${br//\//-}";; esac
sha=$(git rev-parse --short=7 "${1:-HEAD}"); pat="$2"; shift 2 2>/dev/null
until gh api repos/Masuku4129/kira_os_/actions/runs --jq '.workflow_runs[] | select(.head_sha|startswith("'$sha'")) | .status' 2>/dev/null | head -1 | grep -q completed; do sleep 20; done
git fetch -q --depth 1 origin +refs/heads/$cb:refs/remotes/origin/$cb
s=$(git show origin/$cb:latest/SUMMARY.md)
echo "$s" | head -3 | tail -1
echo "OK: $(echo "$s" | grep -c '^OK')"; echo "$s" | grep '^FAIL'
[ -n "$pat" ] && for sc in "$@"; do
    f="latest/vm-$sc/serial.log"; [[ $sc == arm64/* ]] && f="latest/vm-$sc/serial.log"
    echo "== $sc"; git show "origin/$cb:$f" 2>/dev/null | grep -aE "$pat" | head -8
done
exit 0
