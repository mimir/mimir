---
name: verify
description: Pick and run the checks a MimIR change needs - lit, unit tests, the byte-exact output oracles, the x86 small-k sweep, perf A/B, test-py, roundtrip - from what the diff touches. Use before declaring any C++ or .mim change done, when asked to verify/test/check a change, and before handing over a refactor that claims to preserve behavior.
---

# verify

Map the diff to checks, run them, and report outcomes plainly, including what was *not* run and why.

## 1. Classify the change

Start from `git diff HEAD --stat` (plus untracked files); the tree may hold someone else's concurrent work, so only claim checks for your own files.
Never `git stash` to get a baseline; `scripts/oracle.sh rev` uses a scratch worktree instead.

| The diff touches | Run |
| ---------------- | --- |
| anything that builds | `cmake --build build -j16 --target mim mim_all_plugins` and the matching lit tests via `scripts/lit.sh <filter>` |
| a phase, `Analysis`, `Rewriter`, `World`, normalizers, a plugin's C++ | full `scripts/lit.sh` + optimizer oracle (`opt`); add `ll` mode when codegen could change |
| a refactor meant to preserve behavior | the oracle is mandatory: byte-identical or explained |
| `src/mim/ast/` (lexer, parser, emit) | `ast` oracle + `noopt` oracle + `test-roundtrip`; then the `sync-syntax` skill if the surface syntax changed |
| `src/mim/dump.cpp` | `dump5` oracle + `cmake --build build --target test-roundtrip` |
| `include/mim/*.h` signatures, `py/`, a `.mim` surface `py/` mirrors | `cmake --build build --target test-py` (Linux CI does not build Python) |
| library APIs covered by `test/*.cpp` | `ctest --test-dir build --output-on-failure` |
| `src/pbqp/` | `build/bin/mim-pbqp-test` |
| `src/mim/plug/x86/` | `scripts/lit.sh x86/` + `scripts/x86_ksweep.sh`, then the `x86-status` skill |
| `nest.cpp`/`schedule.cpp` | `nest` and `ll` oracles besides `opt` |
| hot paths (`Def`, `World`, hashing, `Checker`, `Zonker`, SEO) or a claimed speedup | `scripts/perf_ab.sh` on a Release tree |
| `lit/*.mim` only | `scripts/lit.sh <those tests>` (re-stages for you) |
| `CMakeLists.txt`, plugin registration | a build plus full lit; `build-static` if plugin entry points changed |

## 2. Oracles need a baseline from *before* the change

- If you are about to edit and the binary is current: `scripts/oracle.sh snap base -m <modes>` first.
- If the edits are already in: `scripts/oracle.sh rev HEAD -m <modes>` builds `HEAD` in a scratch worktree (same build type and compiler) and diffs against it; it caches the build per commit.
- Afterwards: rebuild, then `scripts/oracle.sh diff base`.
- A full run over the 618 inputs takes seconds per mode on a Debug tree with 16 jobs; `rev` first builds `mim` and the plugins once per commit.
- Any difference in a supposedly pure refactor usually means the order of side-effecting `rewrite()` calls changed; find out before reporting.

Perf: `scripts/perf_ab.sh snap before` on the Release tree before the change, rebuild `build-release`, then `scripts/perf_ab.sh cmp before live`.
Report instruction deltas, never wall time.

## 3. Report

- One line per check: what ran, the result, and for failures the test names plus the first relevant lines.
- Distinguish pre-existing failures (also fail on `HEAD`) from new ones; `scripts/oracle.sh rev` and `scripts/lit.sh` on a worktree settle this.
- Say which checks were skipped and why (e.g. "Windows path, only CI can verify").
- Don't commit.
