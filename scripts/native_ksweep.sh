#!/usr/bin/env bash
# Runs the native allowlist under small register counts; each test's expected exit codes are the oracle.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/native_ksweep.sh [-k "2 3 4 6"] [FILTER...]

For each k, runs every lit test that compiles with `-X %native:o=` (or, on x86-64, `-X x86:o=`) through
`scripts/lit.sh --param <backend>_k=<k>`, which adds `-X <backend>:k=<k>` to those RUN lines (see lit/lit.cfg.py).
The backend is the host's: `x86` on x86-64, `arm` on AArch64.
A FILTER narrows the tests further (regex over test paths).
Exits with 1 if any k fails; the per-k logs are <build>/lit-<backend>-k<k>.log.
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

case "$(uname -m)" in
    x86_64 | amd64) backend=x86 patterns=(-e '-X %native:o=' -e '-X x86:o=') ;;
    aarch64 | arm64) backend=arm patterns=(-e '-X %native:o=') ;;
    *) echo "native_ksweep: no native backend for $(uname -m)" >&2; exit 1 ;;
esac

# No mapfile: macOS ships bash 3.2.
tests=()
while IFS= read -r t; do tests+=("$t"); done < <(cd "$root/lit" && grep -rl --include='*.mim' "${patterns[@]}" . | sed 's|^\./||' | LC_ALL=C sort)
if ((${#filters[@]})); then
    re="$(IFS='|'; echo "${filters[*]}")"
    kept=()
    while IFS= read -r t; do kept+=("$t"); done < <(printf '%s\n' ${tests[@]+"${tests[@]}"} | grep -E -- "$re" || true)
    tests=(${kept[@]+"${kept[@]}"})
fi
((${#tests[@]})) || { echo "native_ksweep: no tests" >&2; exit 1; }
filter="$(printf '%s\n' "${tests[@]}" | sed 's/[.]/[.]/g; s/^/(^|[ \/])/; s/$/$/' | paste -sd'|' -)"

status=0
for k in $ks; do
    echo "== k = $k (${#tests[@]} tests)"
    if "$root/scripts/lit.sh" -- --param "${backend}_k=$k" --filter "$filter"; then :; else status=1; fi
    cp "$build/lit.log" "$build/lit-$backend-k$k.log"
done
exit $status
