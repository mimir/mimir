#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <remote> <commit>" >&2
    exit 1
fi

remote=$1
commit=$2

if ! git diff --quiet --ignore-submodules=all || ! git diff --cached --quiet --ignore-submodules=all; then
    echo "error: checkout.sh requires a clean worktree" >&2
    exit 1
fi

git fetch --recurse-submodules=yes "$remote" "$commit"
git checkout --detach FETCH_HEAD
git submodule update --init --recursive
