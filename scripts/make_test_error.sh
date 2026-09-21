#!/usr/bin/env bash

set -euo pipefail

binary=./build/bin/mim-test

if [ "${1-}" = "-b" ]; then
    if [ "$#" -lt 3 ]; then
        echo "usage: $0 [-b <test-binary>] <test-case>" >&2
        exit 1
    fi
    binary=$2
    shift 2
fi

if [ "$#" -ne 1 ]; then
    echo "usage: $0 [-b <test-binary>] <test-case>" >&2
    exit 1
fi

remote=$(git config --get remote.origin.url)
commit=$(git rev-parse HEAD)
filter=$1

printf '```sh\n'
printf './scripts/checkout.sh %q %q && ' "$remote" "$commit"
printf '%q --test-case=%q\n' "$binary" "$filter"
printf '```\n'
