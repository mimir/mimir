#!/usr/bin/env bash
# Runs the x86 allowlist under small register counts; each test's expected exit codes are the oracle.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/x86_ksweep.sh [-k "2 3 4 6"] [FILTER...]

For each k, runs every lit test that compiles with `-X x86:o=` through `scripts/lit.sh --param x86_k=<k>`,
which adds `-X x86:k=<k>` to those RUN lines (see lit/lit.cfg.py).
A FILTER narrows the tests further (regex over test paths).
Exits with 1 if any k fails; the per-k logs are <build>/lit-x86-k<k>.log.
EOF
    exit "${1:-0}"
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${MIM_BUILD_DIR:-$root/build}"
ks="2 3 4 6"
filters=()
while (($#)); do
    case "$1" in
        -h | --help) usage ;;
        -k) ks="$2"; shift ;;
        *) filters+=("$1") ;;
    esac
    shift
done

mapfile -t tests < <(cd "$root/lit" && grep -rl --include='*.mim' -e '-X x86:o=' . | sed 's|^\./||' | LC_ALL=C sort)
if ((${#filters[@]})); then
    re="$(IFS='|'; echo "${filters[*]}")"
    mapfile -t tests < <(printf '%s\n' "${tests[@]}" | grep -E -- "$re")
fi
((${#tests[@]})) || { echo "x86_ksweep: no tests" >&2; exit 1; }
filter="$(printf '%s\n' "${tests[@]}" | sed 's/[.]/[.]/g; s/^/(^|[ \/])/; s/$/$/' | paste -sd'|')"

status=0
for k in $ks; do
    echo "== k = $k (${#tests[@]} tests)"
    if "$root/scripts/lit.sh" -- --param "x86_k=$k" --filter "$filter"; then :; else status=1; fi
    cp "$build/lit.log" "$build/lit-x86-k$k.log"
done
exit $status
