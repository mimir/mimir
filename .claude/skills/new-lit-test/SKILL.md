---
name: new-lit-test
description: Write a new lit test for MimIR in the right shape - refly assertions for normalizers, FileCheck for phases/diagnostics/codegen, run lines for executables with ll and x86 legs - then stage and run it, and mutation-check it. Use whenever adding a regression or feature test under lit/ or extra/<plugin>/lit/.
---

# new-lit-test

Read `lit/AGENTS.md` first; it holds the traps this skill relies on.

## 1. Pick the shape

| Property under test | Shape | Model |
| ------------------- | ----- | ----- |
| construction-time normalization | `// RUN: %mim %s` plus `let _ = refly.equiv.struc_eq (lhs, rhs);` (or `struc_ne`, `refly.check_bot`), a `lam` around free variables | `lit/core/` normalizer tests |
| a phase's result | `// RUN: %mim -p opt %s -o - \| %FileCheck %s` (or a local `_compile` with only the phases under test) | a test in the phase's directory |
| a diagnostic | `// RUN: (! %mim %s 2>&1) \| %FileCheck %s`; if mim must exit 0: `lit/error/warn_decl_arg.mim` | `lit/error/` |
| runtime behavior | `-p ll` leg: compile, `clang`, run, `test $? -eq N`; add the x86 leg below if the program is integer-only | `lit/fun_main_argv.mim` |
| x86 backend stage | a local `_compile` with only the stages needed; `-o -` round trip with `--no-opt` | `lit/x86/` |

x86 leg for an executable (only if `-p opt … -p x86` compiles and runs it correctly):

```
// RUN: %if x86_64-linux %{ %mim -p opt %s -p x86 -X x86:o=%t.s %}
// RUN: %if x86_64-linux %{ clang %t.s -o %t.x86 %}
// RUN: %if x86_64-linux %{ %t.x86 <args>; test $? -eq <N> %}
```

Keep `-p x86 -X x86:o=` adjacent: `--param x86_k` and `scripts/x86_ksweep.sh` rely on that spelling.

## 2. Write it

- Follow `.github/instructions/mim-style.instructions.md`; new `main`s take the dependent `argv` from `lit/AGENTS.md`.
- Name the file after the property, in the directory of the plugin or phase under test; keep it minimal.
- FileCheck: `CHECK-DAG` across top-level Lams, captured names instead of gids, `{{(eta_)?}}` for Debug-only prefixes.
- No `/dev/null` outputs, no `$?` outside the `test $? -eq N` idiom, no `VAR=value` prefixes.
- Leave `-p ll` off programs with runtime-size packs.

## 3. Run and check it

1. `scripts/lit.sh <path relative to lit/>` (it re-stages).
2. Mutation check: flip one expectation (`struc_eq` ↔ `struc_ne`, the expected exit code, one CHECK line) and confirm the test fails; then restore it.
   A refly test whose expected side is spelled with the axioms under test passes vacuously.
3. For a regression test, confirm it fails without the fix (`scripts/oracle.sh rev`'s worktree under `${TMPDIR:-/tmp}/mim-oracle/wt-*` has a `HEAD` build).
4. `cd lit && ./lit ../build/lit --param roundtrip=mim -a --filter <name>`; add `// UNSUPPORTED: roundtrip` only if the input needs RUN-line flags to compile.
5. With an x86 leg: `scripts/x86_ksweep.sh <name>`.
