#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${MIM_BUILD_DIR:-$root/build}"

# Accept "main_loop.mim", "lit/main_loop.mim", or "extra/<plugin>/lit/foo.mim".
# Options, `key=value` words, and everything after `--` pass through to lit.
filters=()
opts=()
while (($#)); do
    case "$1" in
        --) shift; opts+=("$@"); break ;;
        -* | *=*) opts+=("$1") ;;
        *)
            a="${1#lit/}"
            [[ $a =~ ^extra/([^/]+)/lit/(.*)$ ]] && a="extra/${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
            filters+=("$a")
            ;;
    esac
    shift
done
filter="$(IFS='|'; echo "${filters[*]-}")"

cmake --build "$build" --target mim_lit_tests >/dev/null

exec "$root/lit/lit" "$build/lit" -a ${opts[@]+"${opts[@]}"} --filter "$filter"
