#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <lit-filter>" >&2
    exit 1
fi

remote=$(git config --get remote.origin.url)
commit=$(git rev-parse HEAD)

printf './scripts/checkout.sh %q %q && ./scripts/probe.sh' "$remote" "$commit"
for arg in "$@"; do
    printf ' %q' "$arg"
done
printf '\n'
