#!/usr/bin/env bash

set -euo pipefail

binary=./build/bin/mim-gtest

if [ "${1-}" = "-b" ]; then
    if [ "$#" -lt 3 ]; then
        echo "usage: $0 [-b <gtest-binary>] <gtest-filter>" >&2
        exit 1
    fi
    binary=$2
    shift 2
fi

if [ "$#" -ne 1 ]; then
    echo "usage: $0 [-b <gtest-binary>] <gtest-filter>" >&2
    exit 1
fi

remote=$(git config --get remote.origin.url)
commit=$(git rev-parse HEAD)
filter=$1

printf './scripts/checkout.sh %q %q && ' "$remote" "$commit"
printf '%q --gtest_filter=%q --gtest_break_on_failure\n' "$binary" "$filter"
