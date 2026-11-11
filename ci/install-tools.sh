#!/usr/bin/env bash
# Prefer runner/cached package indexes and .deb archives; retry transient mirror/DNS failures.
set -euo pipefail
if (($# == 0)); then echo "usage: $0 package..." >&2; exit 2; fi
missing=()
for package in "$@"; do
    status=$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)
    [[ "$status" == "install ok installed" ]] || missing+=("$package")
done
(("${#missing[@]}")) || exit 0

# First use cached indexes/archives. If a mirror is temporarily unreachable,
# refresh indexes and retry a bounded number of times instead of failing once.
for attempt in 1 2 3; do
    if sudo apt-get install -y -q --no-install-recommends \
        -o Acquire::Retries=2 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15 \
        "${missing[@]}"; then
        exit 0
    fi
    if (( attempt < 3 )); then
        sudo apt-get update -q \
            -o Acquire::Retries=2 -o Acquire::http::Timeout=15 -o Acquire::https::Timeout=15 || true
        sleep "$((attempt * 5))"
    fi
done
echo "Unable to install required packages after 3 attempts: ${missing[*]}" >&2
exit 1
