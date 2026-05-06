# MimIR Copilot Instructions

## Build, test, and lint commands

- Configure a normal local dev build with tests and examples enabled: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMIM_BUILD_EXAMPLES=ON`.
- Build everything from that tree with `cmake --build build -j$(nproc)`.
- Build a specific target when you only need part of the tree: `cmake --build build --target mim`, `cmake --build build --target libmim`, or `cmake --build build --target mim_all_plugins`.
- Run the staged `lit` regression suite with `cmake --build build --target lit`.
- Run a single `lit` test with `cd lit && ./lit ../build/lit -a --filter type_infer.mim`.
- If the build directory is literally named `build`, `cd lit && ../scripts/probe.sh type_infer.mim` is the shortest way to repro one `lit` test.
- Run all GoogleTests with `ctest --test-dir build --output-on-failure`.
- Run a single discovered GoogleTest with `ctest --test-dir build -R dependent_extract --output-on-failure`.
- Run one gtest binary directly with a GoogleTest filter, for example `build/bin/mim-gtest --gtest_filter='World.dependent_extract'` or `build/bin/mim-regex-gtest --gtest_filter='Automaton.DFA'`.
- Formatting/linting is driven by pre-commit hooks in `.pre-commit-config.yaml`; use `pre-commit run --all-files` for the full set or `pre-commit run clang-format --all-files` when you only need formatting.

## High-level architecture

- `libmim` in `src/mim/` is the core library.
  It contains the IR (`Def` and friends), `World`, the AST frontend (`ast/*`), rewriting, checks, passes, phases, dumping, and utility code.
- `Driver` is the central runtime object.
  It owns `Flags`, `Log`, the current `World`, plugin search paths, loaded plugin handles, and the registries for normalizers, stages, and backends.
- The `mim` CLI in `src/mim/cli/main.cpp` is thin glue around `Driver`.
  It parses Mim source into an AST, compiles it into the `World`, loads `compile` and optionally `opt` by default, then runs cleanup or the optimization pipeline before emitting Mim, DOT, LLVM IR, or S-expressions.
- Plugins are split across two artifacts with the same name: a `<plugin>.mim` file and a `libmim_<plugin>` module.
  The `.mim` file declares annexes/axioms and is also used to autogenerate plugin headers and docs, while the shared module exports `mim_get_plugin` to register normalizers, stages, and backends.
- In-tree plugins live under `src/mim/plug/*`.
  Third-party plugins dropped into `extra/*/CMakeLists.txt` are auto-discovered at configure time, and any `extra/<plugin>/lit/*.mim` tests are automatically staged into the main `lit` target.
- Optimization is phase-driven rather than a loose pass list.
  `optimize(World&)` looks for compilation entry points such as `_compile`, resolves the stage from the loaded plugin registry, and runs a `Phase`, `RWPhase`, or `PhaseMan` pipeline over the whole world.
- `RWPhase` rebuilds the old world into a new inherited world and swaps them at the end.
  `Analysis` and `PhaseMan` supply the fixed-point infrastructure used by optimization pipelines.
- `src/automaton/` is a separate static library used by the regex subsystem and its tests.
- Tests are split into two layers.
  `lit/` covers end-to-end Mim CLI behavior using `RUN:` lines and `FileCheck`, while `gtest/*.cpp` exercises library APIs directly and produces the `mim-gtest` and `mim-regex-gtest` executables.

## Key conventions

- Preserve the core IR split between immutable and mutable `Def`s.
  Non-binder expressions are hash-consed and normalized on construction, while binders and other mutables are created first and then filled in to support recursion and variables.
- Terms and types live in the same `World` graph.
  When changing construction, rewriting, or type logic, assume type-level computations participate in the same normalization and sharing machinery as ordinary terms.
- Treat world roots carefully.
  Annexes and external mutables are the roots that analyses and `RWPhase`-based rewrites traverse, and `Cleanup` removes anything not reachable from those roots.
- Prefer the phase infrastructure over adding new ad hoc whole-program traversals.
  The current docs explicitly treat `Phase`/`RWPhase` as the active pipeline model and describe the old pass machinery as deprecated.
- Mim source in docs and tests should use the primary UTF-8 surface syntax and parenthesized domain groups, preferring group patterns, for example `lam foo (x: X, a b c: T) (y: Y): Z = ...`.
- Plugin names are constrained by the runtime encoding and CMake helpers.
  They may use only letters, digits, and underscores, and must be at most 8 characters long.
- When adding or changing plugins, keep both halves in sync.
  The `.mim` file defines the public annex surface, and the C++ module provides the runtime registrations that make those annexes executable.
- If plugin loading behavior matters, remember that `Driver` searches explicit `--plugin-path` entries first, then `MIM_PLUGIN_PATH`, then the executable-relative and install-relative `lib/mim` directories.
