---
applyTo: "**/*.mim"
---

General Mim guidelines for this repository.
Use the primary UTF-8 surface syntax.

- 4 spaces per indentation level; deliberate alignment may differ.
- `snake_case` for term-level names (functions, lambdas, binders, lets, pattern bindings), `CamelCase` for type-level names (types, type constructors).
  Don't mix up the two roles.
- Annotations: exactly one space after the colon, none before — `lam foo (a: A, b: B): R = ...`, not `a:A`, `a : A`, or `) :R`.
  Extra spaces for column alignment are fine.
- Literal ascriptions are tight: `⊤:Nat`, `0.0:%math.F64`.
  For integers prefer the suffix form: `3I32` over `3:I32`.
- In a parameter list prefer `()`-patterns over `[]` — but only when every element is a named binding or nested pattern, e.g. `con f (mem: %mem.M 0, x: I32)`.
  Keep `[]` when the domain has unnamed type elements (`[mem: %mem.M 0, I32, I32]` cannot be a `()`-pattern) and for a `ccon`'s type list, whose brackets denote a type rather than a pattern.
- Prefer group patterns: `(x y: T)` for `(x: T, y: T)`.
  Exception: don't group fields of a named sigma extracted by name (`s#x`), since grouping drops the field names.
- Prefer `Cn X`/`Fn X` over `Cn [X]`/`Fn [X]` for a single argument.
  Keep brackets for several arguments (`Cn [X, Y]`) or when the single argument is an application (`Cn [%mem.M 0]`).
- Shadow one name instead of numbering: `let (mem, x) = f (mem, ...); let (mem, y) = g (mem, ...)` rather than `mem0`/`mem1`/`mem2`.
  Numbered names are fine for distinct sibling parameters of one binder.
- No generated gid-suffixed identifiers (`x_535733`, `mem_19234`) in checked-in code; name them `x`, `mem`.
- Comment sparingly; see the *Comments* section in `.github/copilot-instructions.md`.
