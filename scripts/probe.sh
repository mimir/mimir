#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Accept either "main_loop.mim" or "lit/main_loop.mim"
args=()
for a in "$@"; do
    args+=("${a#lit/}")
done

exec "$root/lit/lit" "$root/build/lit" -a --filter "${args[@]}"
