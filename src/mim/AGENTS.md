# libmim

Plugin-specific notes live in `plug/AGENTS.md` and below; phase-specific ones in `phase/AGENTS.md`.

## Core IR

- A `Var` has no ops: its binder sits in the `Def` union (`binder_`), so `Def::equal` needs its explicit Var branch and `Var::rebuild_` is unreachable.
  A rewriter that bypasses `Rewriter` must short-circuit Vars before falling back to `def->rebuild(...)`.
- `Def::nests` is the lg26 nesting relation and is strict: `f->nests(def)` is false for a `def` that only uses `f`'s own Var.
  Use it for scope questions instead of hand-rolled visibility checks.
- A *failing* `Checker::alpha` is load-bearing: its first act is `zonk_mut()`, which rewires mutables in place.
  Only a ground `Def` (`Def::is_ground()`) may be rejected early.
- `Zonker::lookup` hits iff `new == old || new->isa_mut() || !new->needs_zonk()`; dropping the `new == old` clause made `hlo` 3.7× slower.
- `Nest` computes everything eagerly in its ctor; don't reintroduce lazy or `mutable` caches.
  Dominance is slated for removal, not for being made lazy.
- `GIDHash`/`SeaHash` are measured to be good and marked `is_avalanching`; never swap in a bare multiply mix (low bits become a permutation of the gid's).
- `World::inhabitant` is about 0.13% of `vgg16`; caching singleton-ness in a `Def` bit measured as a net loss.
- The one-argument `proj(i)`/`var(i)`/`arg(i)` are deleted on purpose (a collapsed Pack makes `proj(0)` silently wrong); the `t`-variants keep theirs, since peeling a 1-tuple is the calling convention.
- `World::annex(flags)` throws when the plugin is not loaded; `World::reduce(var, arg, i)` must not guard on the whole mutable being set, since the frontend projects out of a mutable it is still filling.

## Rules (`rule.h`, `rule.cpp`)

- `Rho`/`Rule`/`Fire` mirror `Pi`/`Lam`/`App`; a Rule's Var binds its meta variables (`Rule::meta`), which matching determines.
  `rule I (dom)* (meta): …` is sugar for `anx lam I (dom)* = rl (meta): …`: the curried Lams make a family that whoever uses it instantiates; their parameters are never matched.
- A Rule's type never mentions the Rule's own Var: a mutable `Rho` (`{s: Nat} Idx s ↝ Idx s`) binds the metas its types mention with a Var of its own, which `Rho::instantiate` solves by matching `Rho::dom` against the argument's *type*.
  The frontend abstracts the Rule's Var into that Rho and prunes it to the components the types use; `Rule::dom`/`codom` are instantiated by `Rule::lhs`.
- Patterns are normalized like any term (`a + a` becomes `2 * a`), which is also how they match normalized programs.
- A tuple pattern matches a non-tuple term by its projections, since a tuple of metas is often η-collapsed in the term (`core.wrap.add m x` for a Var `x`).
- The Rule of `rule I …` carries the sym `I`, so every instance of the family keeps that name.

## Analysis and RWPhase (`phase.cpp`)

- `Analysis` iterates sparsely: `touch()` records `curr_mut()` as dirty; a sparse round replays the lattice (skipping mutable keys) and drains the dirty muts in `GIDLt` order; only a quiet *full* round certifies the fixed point, and `finalize()` runs only in full rounds.
- `lattice()` silently creates a fresh ⊤; without that, every `pin()` would force dense rounds.
- `lattice_force` deliberately seeds `map()` too.
- Analysis rounds run under `World::lenient()`, so optimistic substitutions may build ill-typed dependent terms; the final `RWPhase` rewrite stays strict and is the soundness check.
  If a phase throws "not assignable" only under `-p opt`, suspect analysis-time partial substitution.
- Every `RWPhase` coerces at checking positions (App argument, Tuple ops, Lam body); Variants stay opaque to coercion on purpose.
- Annexes are imported lazily from `Driver::library()`; a non-Lam annex is opaque to phases, and annex Lams become ordinary program Lams.
  A phase never alters a non-Lam annex; an opaque `Axm` whose type mentions what a phase rewrites gets `RWPhase::retype`.
- Never test "is new" with `&def->world() == &X`.

## Dumper (`dump.cpp`)

- It stays one file; a split into `dump/` was tried and cost about 290 lines for nothing.
  "Simplify" is measured in net lines.
- A Nom is just `(key, op)`: don't add arguments or provenance to simplify the dumper.
- Rejected after analysis, don't re-propose: one exclusive `DefMap<Place>`, a binder-shape pre-pass, a `printed_deps` assertion or dry-run printer.
- `-o` and `--output-ast` are byte fixpoints over lit (`test-roundtrip`): plain names are picked in output order, traversal is structural (never pointer- or loc-ordered), and only `printed_deps` are walked.
- The dumper runs on a frozen World by design: frozen queries return `nullptr`, and it must degrade gracefully.
- `extern` forward declarations are deliberately no World externals.
- Verify any change with the 5-mode byte oracle (`-o -`, `--no-opt -o -`, `--mim-typed-let -o -`, `--no-opt -a -o -`, `--no-opt --output-dot -` over all lit/extra `.mim`; stdout, stderr, and exit code) plus `test-roundtrip`.

## Frontend (`ast/`)

- `+ - * / %` have no built-in meaning: `a op b` desugars to `` `op (a, b) ``, and the escaped Sym includes the backtick (needed for the `--output-ast` round trip).
- Literal signs are parsed, not lexed (`f -3` subtracts); don't reintroduce sign lexing, it makes `a-1` a silent application.
- An `axm F` and a `mod F` in one scope are a redeclaration error, so an axiom cannot have child annexes under its own name.
- An `anx let` may shadow a plain declaration (`priv nom Tensor …; pub anx let Tensor = Tensor;`) but not another annex, whose flags would collide in `World::Annexes::attach`.
- Verify parser refactors by diffing `mim --output-ast -` over all lit/extra `.mim`; no lit test covers the AST stream.
