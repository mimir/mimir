#!/usr/bin/env bash
# Runs the lit suite (or a filtered part) on freshly staged copies, memory-capped, with a quiet summary.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/lit.sh [FILTER...] [-- LIT-OPTION...]

Re-stages lit/*.mim (target mim_lit_tests), then runs lit quietly with the full log in <build>/lit.log.
A FILTER is a regex over test paths (e.g. 'x86/', 'clos/|matrix/'); several are or-ed.
Options and `key=value` words pass through to lit, e.g. `--param roundtrip=mim` or `--param x86_k=3`.

environment: MIM_BUILD_DIR (default build), MIM_JOBS (default min(16, nproc)), MIM_VMEM (KiB, default 4000000)
EOF
    exit "${1:-0}"
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${MIM_BUILD_DIR:-$root/build}"
n="$(nproc)"
jobs="${MIM_JOBS:-$((n < 16 ? n : 16))}"

filters=()
opts=()
while (($#)); do
    case "$1" in
        -h | --help) usage ;;
        --) shift; opts+=("$@"); break ;;
        -* | *=*) opts+=("$1") ;;
        *) filters+=("${1#lit/}") ;;
    esac
    shift
done
((${#filters[@]})) && opts+=(--filter "$(IFS='|'; echo "${filters[*]}")")

cmake --build "$build" --target mim_lit_tests >/dev/null

log="$build/lit.log"
set +e
(ulimit -v "${MIM_VMEM:-4000000}" && "$root/lit/lit" "$build/lit" -j "$jobs" -v ${opts[@]+"${opts[@]}"}) >"$log" 2>&1
rc=$?
set -e
grep -E '^(FAIL|XPASS|UNRESOLVED|TIMEOUT): ' "$log" || true
sed -n '/^Testing Time/,$p' "$log" | grep -v '^$' || tail -5 "$log"
echo "log: $log"
exit $rc
