---
applyTo: "**/*.mim"
---

When reviewing Mim source files in this repository, treat the following as coding-style requirements:

- Prefer 4 spaces for one indentation level, but do not flag deliberate alignment that uses different spacing for readability.
- Prefer `snake_case` for value-level names such as functions, lambdas, binders, local lets, pattern-bound values, and similar term-level identifiers.
- Prefer `CamelCase` for type-level names such as types, type constructors, and similar identifiers that denote type-level entities.
- Flag naming that mixes these roles incorrectly, even if the Mim code is otherwise valid.
- Prefer `()`-style patterns over `[]`-style patterns in functions.
- Prefer group patterns when possible, e.g., `(x y: T)` as shorthand for `(x: T, y: T)`.
