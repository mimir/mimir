---
applyTo: "**/*.h,**/*.hh,**/*.hpp,**/*.hxx,**/*.c,**/*.cc,**/*.cpp,**/*.cxx"
---

General C++ guidelines for this repository.

- `.clang-format` is the source of truth for formatting (indentation, spacing, alignment, wrapping, braces).
  Don't hand-format what it already governs.
- Containers, in order of preference:
  1. MimIR types/aliases (`mim::Vector`, `DefVec`, `GIDMap`, `GIDSet`, `GIDNodeMap`, `GIDNodeSet`, `LamMap`, `LamSet`, `DefMap`, `DefSet`, `MutMap`, `MutSet`, `VarMap`, `VarSet`, `Muts`, `Vars`, plus aliases defined near the code at hand),
  2. `ankerl::unordered_dense` containers,
  3. `std::*` only when nothing above fits, or for value types like `std::array`, `std::span`, `std::string`, `std::tuple`, `std::pair`.
- Unpack a fixed number of projections with a single `projs<N>()` plus a structured binding — `auto [x, y] = def->projs<2>();`, not repeated `def->proj(2, i)`.
  Use the typed `projs<N>(...)` overload when a conversion lambda is needed.
- Prefer the `MIM_PROJ`-generated helpers over extracting an intermediate `Def*` first: `app->args<N>()` instead of `app->arg()->projs<N>()`, `def->vars<N>()`, etc.
- Prefer a nested `if (auto x = ...)` chain over early-return guards when the whole function is one structural test:
  ```cpp
  if (auto outer = type->isa<Pi>())
      if (auto inner = outer->codom()->isa<Pi>()) return inner->dom() == inner->codom();
  return false;
  ```
  not a sequence of `auto x = ...; if (!x) return false;`.
- Never check a `Def*` argument for `nullptr` inside a helper; that is the caller's responsibility.
  A helper such as `Pi::isa_cn` or `Idx::isa` dereferences its argument, and the caller guards wherever a
  `nullptr` can actually arise - `Def::unfold_type` of `Univ`, or a projection in a frozen `World`.
  `Lit::isa` is the one deliberate exception, documented at its declaration.
- In Doxygen comments (`///`, `/** ... */`), prefer one sentence per line over column-filling wraps, so diffs stay readable.
  Deliberate exceptions are fine where a sentence would fragment awkwardly.
- Comment sparingly; see the *Comments* section in `.github/copilot-instructions.md`.
