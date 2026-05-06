---
applyTo: "**/*.h,**/*.hh,**/*.hpp,**/*.hxx,**/*.c,**/*.cc,**/*.cpp,**/*.cxx"
---

When reviewing C++ changes in this repository, treat the following as coding-style requirements:

- The repository already has a `.clang-format`; treat it as the source of truth for C++ formatting details such as indentation, spacing, alignment, wrapping, and brace layout.
- Do not leave review comments for formatting that is already governed by `.clang-format`; focus on the project-specific style rules below and on cases where code clearly does not follow the repository formatter.
- Prefer MimIR container types and aliases first, such as `mim::Vector`, `DefVec`, `GIDMap`, `GIDSet`, `GIDNodeMap`, `GIDNodeSet`, `LamMap`, `LamSet`, `DefMap`, `DefSet`, `MutMap`, `MutSet`, `VarMap`, `VarSet`, `Muts`, and `Vars`, plus similar aliases already defined near the code being changed.
- If there is no fitting MimIR container or alias, prefer Abseil containers over `std::vector`, `std::map`, `std::set`, `std::unordered_map`, `std::unordered_set`, and similar STL containers.
- Only accept `std::*` containers when neither a MimIR container nor an Abseil container is appropriate, or when the code specifically needs standard-library types such as `std::array`, `std::span`, `std::string`, `std::tuple`, or `std::pair`.
- Prefer structured bindings together with `projs<N>()` when unpacking a fixed number of projections. For example, prefer `auto [x, y] = def->projs<2>();` over separate `auto x = def->proj(2, 0);` and `auto y = def->proj(2, 1);`.
- Prefer the repo-specific projection helpers generated via `MIM_PROJ` when they exist, such as `app->args<N>()`, `def->vars<N>()`, and similar `...s<N>()` helpers, instead of first extracting an intermediate `Def*` and then calling `projs<N>()` on it.
- In particular, prefer `app->args<N>()` over `app->arg()->projs<N>()` when unpacking the fixed arguments of an `App`.
- Flag repeated `proj(arity, index)` calls that can be replaced by a single `projs<N>()` call, including typed `projs<N>(...)` overloads when a conversion lambda is needed.
- Flag cases where a `MIM_PROJ`-generated helper would express the same unpacking more directly than raw `proj(...)` or `...()->projs<N>()` chains.
- Call out violations of these rules in review comments even when the code is otherwise correct.
