#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
staged="$root/build/lit/tests"

# Accept "main_loop.mim", "lit/main_loop.mim", or "extra/<plugin>/lit/foo.mim"; options pass through to lit.
filters=()
opts=()
for a in "$@"; do
    case "$a" in
        -*) opts+=("$a") ;;
        *)
            a="${a#lit/}"
            [[ $a =~ ^extra/([^/]+)/lit/(.*)$ ]] && a="extra/${BASH_REMATCH[1]}/${BASH_REMATCH[2]}"
            filters+=("$a")
            ;;
    esac
done
filter="$(IFS='|'; echo "${filters[*]}")"

source_of() {
    if [[ $1 =~ ^extra/([^/]+)/(.*)$ ]]; then
        echo "$root/extra/${BASH_REMATCH[1]}/lit/${BASH_REMATCH[2]}"
    else
        echo "$root/lit/$1"
    fi
}

stage_tree() {
    local src_root="$1" dst_prefix="$2" f rel dst
    [[ -d $src_root ]] || return 0
    while IFS= read -r -d '' f; do
        rel="${f#"$src_root"/}"
        case "${rel##*/}" in CMakeLists.txt | lit.cfg.py | lit.site.cfg.py | lit.site.cfg.py.in) continue ;; esac
        dst="$staged/$dst_prefix$rel"
        if ! cmp -s "$f" "$dst"; then
            mkdir -p "$(dirname "$dst")"
            cp "$f" "$dst"
        fi
    done < <(find "$src_root" -type f -print0)
}

stage_tree "$root/lit" ""
for dir in "$root"/extra/*/lit; do
    [[ -d $dir ]] || continue
    plugin="$(basename "$(dirname "$dir")")"
    stage_tree "$dir" "extra/$plugin/"
done

# Drop staged copies of tests that no longer exist in the source tree.
if [[ -d $staged ]]; then
    while IFS= read -r -d '' f; do
        rel="${f#"$staged"/}"
        [[ -e "$(source_of "$rel")" ]] || rm -f "$f"
    done < <(find "$staged" -type f ! -name .stamp -print0)
fi

exec "$root/lit/lit" "$root/build/lit" -a "${opts[@]}" --filter "$filter"
