---
applyTo: "**/*.mim"
---

When reviewing Mim source files in this repository, treat the following as coding-style requirements:

- Prefer 4 spaces for one indentation level, but do not flag deliberate alignment that uses different spacing for readability.
- Prefer `snake_case` for value-level names such as functions, lambdas, binders, local lets, pattern-bound values, and similar term-level identifiers.
- Prefer `CamelCase` for type-level names such as types, type constructors, and similar identifiers that denote type-level entities.
- Flag naming that mixes these roles incorrectly, even if the Mim code is otherwise valid.
- For binding and return-type annotations, prefer exactly one space after the colon and none before, e.g. `lam foo (a: A, b: B): R = ...` (not `a:A`, `a : A`, or `): R` written as `) :R`). Do not flag additional spaces used for deliberate column alignment.
- For a literal type ascription the tight form `a:T` (no surrounding spaces) is correct, e.g. `⊤:Nat` or `0.0:%math.F64`. For integer literals prefer the suffix form over an ascription, e.g. `3I32` over `3:I32`.
- Prefer `()`-style patterns over `[]`-style patterns in a function's parameter list, but only when every element is a named binding (or a nested pattern), e.g. `con f (mem: %mem.M 0, x: I32)`. Keep `[]` when the domain contains unnamed type elements (e.g. `[mem: %mem.M 0, I32, I32]`, which cannot be a `()`-pattern) and for a `ccon`'s type list (its brackets denote a type, not a pattern).
- Prefer group patterns when possible, e.g., `(x y: T)` as shorthand for `(x: T, y: T)`. Do not group the fields of a named sigma type that are extracted by name (`s#x`), since the group form drops the individual field names.
- Prefer `Cn X` over `Cn [X]` (and `Fn X` over `Fn [X]`) when the continuation/function takes a single argument. Keep the brackets for multiple arguments (`Cn [X, Y]`) or when the single argument is itself an application (`Cn [%mem.M 0]`).
- Prefer reusing a single shadowed name over a numbered chain, e.g. `let (mem, x) = f (mem, ...); let (mem, y) = g (mem, ...)` instead of `mem0`/`mem1`/`mem2`. This does not apply where the numbered names are distinct sibling parameters of one binder.
- Avoid generated, gid-suffixed identifiers such as `x_535733` or `mem_19234` in checked-in examples; give them readable names (`x`, `mem`).
