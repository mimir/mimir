# lit tests

- lit runs the copies staged into `build/lit/`; build `mim_lit_tests` after editing a `lit/*.mim`, or you test a stale file.
- Run the suite with `scripts/lit.sh [FILTER...]`: it re-stages and caps memory, so a runaway `mim` dies with `std::bad_alloc` instead of taking the machine down.
- A normalizer test asserts with `let _ = refly.equiv.struc_eq (lhs, rhs);` (or `struc_ne`, `refly.check_bot`) and a plain `// RUN: %mim %s`; FileCheck is only for phase results, diagnostics, and codegen.
  Spell the expected side without the axioms under test, or it re-normalizes and the check passes vacuously.
- Top-level Lams dump in no portable order: use `CHECK-DAG` across Lams and tie a body to its header with a captured name, not `CHECK-NEXT`.
  The `eta_` prefix exists only in Debug builds: match it as `{{(eta_)?}}`.
- `(! %mim …)` inverts valgrind's exit code too; a diagnostic test that must exit 0 follows `lit/error/warn_decl_arg.mim`.
- Never write output to `/dev/null` (hangs Windows CI); use `-` as in `lit/annex.mim`.
- lit's internal shell knows neither `$?` nor `VAR=value`, so don't add more RUN lines that need the external one.
- The `ll` backend cannot emit runtime-size packs: leave `-p ll` off such tests.
- `-p x86` runs are guarded by `%if x86_64-linux %{ … %}`; keep `-p x86 -X x86:o=` adjacent, since `--param x86_k=<n>` (`scripts/x86_ksweep.sh`) hooks into that spelling.
- New `main`s take a dependent `argv: mem.Ptr0 «core.idx2nat argc; mem.Ptr0 I8»`; read memory of unknown length from C through `mem.assume (p, n)` and pass C element pointers as `mem.decay p`.

Mim surface gotchas:

- `ret` and `rec` are reserved; don't use them as binder names.
- `lam` needs a name; a local function is `lm (x: T): R = e` or `λ`.
- `(a: T, b: T)` in a parameter list is a group that flattens into the domain; nest with `ab: [T, T]`.
- Don't convert `[]` to `()` mechanically: `ccon` lists, annotations, and groups with unnamed components stay `[]`.
- Testing mutually recursive `lam`s needs `@ff`, or a call with a non-literal argument unfolds forever.
- `f -3` is a subtraction and `f *` a multiplication: write `f (-3)` and `f (*)`.
