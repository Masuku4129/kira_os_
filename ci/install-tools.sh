#!/usr/bin/env bash
# Prefer runner/cached package indexes and .deb archives; contact mirrors only when a package is unavailable.
set -euo pipefail
if (($# == 0)); then echo "usage: $0 package..." >&2; exit 2; fi
missing=()
for package in "$@"; do
    status=$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true)
    [[ "$status" == "install ok installed" ]] || missing+=("$package")
done
((${#missing[@]})) || exit 0
if sudo apt-get install -y -q --no-install-recommends -o Acquire::Retries=0 -o Acquire::http::Timeout=8 -o Acquire::https::Timeout=8 "${missing[@]}"; then exit 0; fi
sudo apt-get update -q -o Acquire::Retries=0 -o Acquire::http::Timeout=12 -o Acquire::https::Timeout=12
sudo apt-get install -y -q --no-install-recommends "${missing[@]}"
