#!/usr/bin/env bash
# Byte-exact A/B oracle: runs `mim` over every lit/extra .mim and diffs stdout, stderr, and exit code.
set -euo pipefail

usage() {
    cat <<'EOF'
usage: scripts/oracle.sh snap NAME [options]   record the current build's outputs as NAME
       scripts/oracle.sh diff NAME [options]   compare the current build against NAME
       scripts/oracle.sh rev REV [options]     build REV in a scratch worktree, snap it, and diff against it
       scripts/oracle.sh list                  list snapshots

options:
  -m, --mode M     opt (default), noopt, typed, ascii, dot, ast, ll, nest, dump5 (= opt noopt typed ascii dot),
                   or NAME='ARGS' with {} for the input file; repeatable
  -b, --build DIR  build tree whose bin/mim runs (default: $MIM_BUILD_DIR or build)
  -j N             parallel jobs (default: $MIM_JOBS or min(16, nproc))
  -- FILE...       only these inputs instead of the whole corpus

Snapshots live in $MIM_ORACLE_DIR (default: ${TMPDIR:-/tmp}/mim-oracle).
Each run is capped at $MIM_VMEM KiB of virtual memory (default 4000000) and $MIM_TIMEOUT seconds (default 120).
diff and rev exit with 1 if any output differs.
Snap the baseline *before* editing, with a binary that already has every unrelated fix.
EOF
    exit "${1:-0}"
}

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${MIM_BUILD_DIR:-$root/build}"
store="${MIM_ORACLE_DIR:-${TMPDIR:-/tmp}/mim-oracle}"
n="$(nproc)"
jobs="${MIM_JOBS:-$((n < 16 ? n : 16))}"
export MIM_VMEM="${MIM_VMEM:-4000000}" MIM_TIMEOUT="${MIM_TIMEOUT:-120}"

(($#)) || usage 1
cmd="$1"
shift
case "$cmd" in
    -h | --help) usage ;;
    list) ls -1 "$store" 2>/dev/null | grep -v '^\.' || true; exit 0 ;;
    snap | diff | rev) ;;
    *) usage 1 ;;
esac
(($#)) || usage 1
name="$1"
shift

modes=()
files=()
while (($#)); do
    case "$1" in
        -m | --mode) modes+=("$2"); shift ;;
        -b | --build) build="$(cd "$2" && pwd)"; shift ;;
        -j) jobs="$2"; shift ;;
        --) shift; files=("$@"); break ;;
        -h | --help) usage ;;
        *) echo "oracle: unknown argument '$1'" >&2; usage 1 ;;
    esac
    shift
done
((${#modes[@]})) || modes=(opt)

expanded=()
for m in "${modes[@]}"; do
    if [[ $m == dump5 ]]; then expanded+=(opt noopt typed ascii dot); else expanded+=("$m"); fi
done
modes=("${expanded[@]}")

mode_args() {
    case "$1" in
        opt) echo '-p opt {} -o -' ;;
        noopt) echo '--no-opt {} -o -' ;;
        typed) echo '--mim-typed-let {} -o -' ;;
        ascii) echo '--no-opt -a {} -o -' ;;
        dot) echo '--no-opt --output-dot - {}' ;;
        ast) echo '--output-ast - {}' ;;
        ll) echo '-p opt {} -p ll -X ll:o=-' ;;
        nest) echo '--output-nest - {}' ;;
        *=*) echo "${1#*=}" ;;
        *) echo "oracle: unknown mode '$1'" >&2; exit 1 ;;
    esac
}
mode_dir() { echo "${1%%=*}"; }

corpus() {
    if ((${#files[@]})); then
        for f in "${files[@]}"; do realpath --relative-to="$root" "$f"; done
    else
        local dirs=(lit)
        for d in "$root"/extra/*/lit; do [[ -d $d ]] && dirs+=("${d#"$root"/}"); done
        (cd "$root" && find "${dirs[@]}" -name '*.mim' | LC_ALL=C sort)
    fi
}

# Runs one input; the build tree, its source tree (`__FILE__`), and the scratch cwd are normalized so that two trees compare.
run_one() {
    local mim="$1" args="$2" dest="$3" rel="$4"
    local out="$dest/$rel" cwd
    mkdir -p "$(dirname "$out")"
    cwd="$(mktemp -d)"
    local argv=()
    read -r -a argv <<<"${args//\{\}/$ORACLE_ROOT/$rel}"
    set +e
    (cd "$cwd" && ulimit -v "$MIM_VMEM" && exec timeout "$MIM_TIMEOUT" "$mim" "${argv[@]}") >"$out.out" 2>"$out.err"
    echo $? >"$out.rc"
    set -e
    local tree
    tree="$(dirname "$(dirname "$mim")")"
    sed -i -e "s|$tree|<build>|g" -e "s|$(dirname "$tree")/src/|<src>/|g" -e "s|$cwd|<cwd>|g" "$out.out" "$out.err" || true
    rm -rf "$cwd"
    return 0
}
export -f run_one
export ORACLE_ROOT="$root"

snap() {
    local mim="$1" dest="$2"
    [[ -x $mim ]] || { echo "oracle: no mim at $mim" >&2; exit 1; }
    rm -rf "$dest"
    mkdir -p "$dest"
    printf '%s\n' "${modes[@]}" >"$dest/.modes"
    local list
    list="$(corpus)"
    for m in "${modes[@]}"; do
        local args d
        args="$(mode_args "$m")"
        d="$dest/$(mode_dir "$m")"
        echo "oracle: $(wc -l <<<"$list") inputs, mode $(mode_dir "$m"), $mim" >&2
        xargs -d '\n' -P "$jobs" -I@REL@ bash -c 'run_one "$@"' _ "$mim" "$args" "$d" @REL@ <<<"$list"
    done
}

compare() {
    local base="$1" cur="$2" status=0
    for m in "${modes[@]}"; do
        local d
        d="$(mode_dir "$m")"
        [[ -d $base/$d ]] || { echo "oracle: snapshot has no mode '$d'" >&2; exit 1; }
        local changed total
        changed="$({ diff -rq "$base/$d" "$cur/$d" || true; } | sed -E 's/^Files (.*) and .* differ$/\1/; s/^Only in (.*): (.*)$/\1\/\2/' |
            sed -E "s|^$base/$d/||; s/\.(out|err|rc)$//" | LC_ALL=C sort -u)"
        total="$(find "$cur/$d" -name '*.rc' | wc -l)"
        if [[ -z $changed ]]; then
            echo "$d: $total/$total identical"
            continue
        fi
        status=1
        echo "$d: $(wc -l <<<"$changed") of $total differ"
        local shown=0
        while read -r rel; do
            local rc_base rc_cur
            rc_base="$(cat "$base/$d/$rel.rc" 2>/dev/null || echo -)"
            rc_cur="$(cat "$cur/$d/$rel.rc" 2>/dev/null || echo -)"
            echo "  $rel (rc $rc_base -> $rc_cur)"
            if ((shown < 3)); then
                for ext in out err; do
                    diff -u --label "base/$rel.$ext" --label "cur/$rel.$ext" "$base/$d/$rel.$ext" "$cur/$d/$rel.$ext" 2>/dev/null |
                        head -20 | sed 's/^/    /' || true
                done
                shown=$((shown + 1))
            fi
        done <<<"$changed"
        echo "  full outputs: $base/$d vs. $cur/$d"
    done
    return $status
}

build_rev() {
    local sha="$1" wt="$store/wt-$1"
    if [[ ! -x $wt/build/bin/mim ]]; then
        [[ -d $wt ]] || git -C "$root" worktree add --detach "$wt" "$sha" >&2
        git -C "$wt" submodule update --init --recursive >&2
        local cache="$build/CMakeCache.txt" type gen cxx
        type="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache")"
        gen="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache")"
        cxx="$(sed -n 's/^CMAKE_CXX_COMPILER:[A-Z]*=//p' "$cache")"
        cmake -S "$wt" -B "$wt/build" -G "$gen" -DCMAKE_BUILD_TYPE="$type" -DCMAKE_CXX_COMPILER="$cxx" \
            -DBUILD_TESTING=OFF -DMIM_BUILD_PYTHON=OFF -DMIM_BUILD_DOCS=OFF -DMIM_BUILD_EXAMPLES=OFF >&2
        cmake --build "$wt/build" -j "$jobs" --target mim mim_all_plugins >&2
    fi
    echo "$wt/build/bin/mim"
}

case "$cmd" in
    snap) snap "$build/bin/mim" "$store/$name" ;;
    diff)
        [[ -d $store/$name ]] || { echo "oracle: no snapshot '$name'" >&2; exit 1; }
        ((${#modes[@]} == 1)) && [[ ${modes[0]} == opt ]] && mapfile -t modes <"$store/$name/.modes"
        snap "$build/bin/mim" "$store/.current"
        compare "$store/$name" "$store/.current"
        ;;
    rev)
        sha="$(git -C "$root" rev-parse --short=12 "$name^{commit}")"
        mim="$(build_rev "$sha")"
        snap "$mim" "$store/rev-$sha"
        snap "$build/bin/mim" "$store/.current"
        compare "$store/rev-$sha" "$store/.current"
        ;;
esac
