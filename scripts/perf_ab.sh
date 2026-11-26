#!/usr/bin/env bash
# Compares user-space instruction counts of two staged mim builds; wall time is too noisy for A/B.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/perf_ab.sh snap NAME [--build DIR]       copy DIR/{bin,lib} (default build-release) as NAME
       scripts/perf_ab.sh cmp A B [-r N] [-w W]...      compare two snapshots; B may be `live` for DIR itself
       scripts/perf_ab.sh list

workloads (-w, repeatable; default: hlo vgg16):
  hlo    mim -p opt -p gpu lit/tensor/hlo.mim -p nvptx
  vgg16  mim lit/tensor/vgg16.mim -p opt -o -
  NAME='ARGS'  custom arguments, run from the repository root

Each workload runs N times per build (default 3) and reports the median of `perf stat -e instructions:u`.
Use a Release tree: Debug enables MIM_ENABLE_CHECKS and misleads.
Snapshots live in $MIM_PERF_DIR (default: ${TMPDIR:-/tmp}/mim-perf).
EOF
    exit "${1:-0}"
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-release"
store="${MIM_PERF_DIR:-${TMPDIR:-/tmp}/mim-perf}"

(($#)) || usage 1
cmd="$1"
shift
case "$cmd" in
    -h | --help) usage ;;
    list) ls -1 "$store" 2>/dev/null || true; exit 0 ;;
    snap)
        (($#)) || usage 1
        name="$1"
        shift
        [[ ${1-} == --build ]] && build="$(cd "$2" && pwd)"
        grep -q '^CMAKE_BUILD_TYPE:STRING=Release' "$build/CMakeCache.txt" || echo "perf_ab: warning: $build is no Release tree" >&2
        rm -rf "${store:?}/$name"
        mkdir -p "$store/$name"
        cp -a "$build/bin" "$build/lib" "$store/$name/"
        echo "perf_ab: $build -> $store/$name"
        exit 0
        ;;
    cmp) (($# >= 2)) || usage 1 ;;
    *) usage 1 ;;
esac

a="$1" b="$2"
shift 2
reps=3
workloads=()
while (($#)); do
    case "$1" in
        -r) reps="$2"; shift ;;
        -w) workloads+=("$2"); shift ;;
        --build) build="$(cd "$2" && pwd)"; shift ;;
        *) usage 1 ;;
    esac
    shift
done
((${#workloads[@]})) || workloads=(hlo vgg16)

tree() {
    if [[ $1 == live ]]; then echo "$build"; else echo "$store/$1"; fi
}

workload_args() {
    case "$1" in
        hlo) echo "-p opt -p gpu $root/lit/tensor/hlo.mim -p nvptx" ;;
        vgg16) echo "$root/lit/tensor/vgg16.mim -p opt -o -" ;;
        *=*) echo "${1#*=}" ;;
        *) echo "perf_ab: unknown workload '$1'" >&2; exit 1 ;;
    esac
}

# Prints the median instruction count of `reps` runs.
measure() {
    local t="$1" args="$2" cwd counts=()
    cwd="$(mktemp -d)"
    local argv=()
    read -r -a argv <<<"$args"
    for ((i = 0; i < reps; ++i)); do
        local c
        c="$(cd "$cwd" && LD_LIBRARY_PATH="$t/lib" perf stat -x, -e instructions:u "$t/bin/mim" "${argv[@]}" 2>&1 >/dev/null |
            awk -F, '/instructions/ { print $1 }')"
        [[ $c =~ ^[0-9]+$ ]] || { echo "perf_ab: run failed in $t: $args" >&2; rm -rf "$cwd"; exit 1; }
        counts+=("$c")
    done
    rm -rf "$cwd"
    printf '%s\n' "${counts[@]}" | sort -n | sed -n "$(((reps + 1) / 2))p"
}

ta="$(tree "$a")" tb="$(tree "$b")"
for t in "$ta" "$tb"; do [[ -x $t/bin/mim ]] || { echo "perf_ab: no mim in $t" >&2; exit 1; }; done

printf '%-10s %16s %16s %9s\n' workload "$a" "$b" delta
for w in "${workloads[@]}"; do
    args="$(workload_args "$w")"
    ia="$(measure "$ta" "$args")"
    ib="$(measure "$tb" "$args")"
    awk -v w="${w%%=*}" -v a="$ia" -v b="$ib" 'BEGIN { printf "%-10s %15.1fM %15.1fM %+8.2f%%\n", w, a / 1e6, b / 1e6, (b - a) * 100 / a }'
done
