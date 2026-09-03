# MimIR AI Instructions

## Build & test

Assume a `build/` tree configured with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DMIM_BUILD_EXAMPLES=ON`.

| Task            | Command                                                                                     |
| --------------- | ------------------------------------------------------------------------------------------- |
| Build all       | `cmake --build build -j$(nproc)`                                                            |
| Build part      | same with `--target mim` / `libmim` / `mim_all_plugins`                                     |
| All tests       | `cmake --build build --target test-all`                                                     |
| `lit` suite     | `cmake --build build --target lit`                                                          |
| One `lit` test  | `cd lit && ../scripts/probe.sh type_infer.mim` or `./lit ../build/lit -a --filter type_infer.mim` |
| All unit tests  | `ctest --test-dir build --output-on-failure`                                                |
| One unit test   | `build/bin/mim-test -tc='World: dependent extract'`                                         |
| Format/lint     | `pre-commit run --all-files` (or `pre-commit run clang-format --all-files`)                  |

`lit` runs against a *staged* copy of `lit/`, so build the `lit` target (or `mim_lit_tests`) after editing a test — otherwise you test a stale file.

## Architecture

- `libmim` (`src/mim/`) is the core library: the IR (`Def`, `World`), the AST frontend (`ast/*`), rewriting, checks, phases, dumping, and utilities.
- `Driver` is the central runtime object.
  It owns `Flags`, `Log`, the current `World`, plugin search paths and handles, and the normalizer/stage/backend registries.
- The `mim` CLI (`src/mim/cli/main.cpp`) is thin glue around `Driver`: parse Mim → compile into the `World` → `optimize(World&)` → emit whatever `--output-*` asks for.
  Plugins are loaded on demand via `plugin` directives or `-p`/`--plugin`; `--output-h`/`--output-py` bootstrap plugin headers and exit before compilation.
  Plugin lookup order: current directory, `--plugin-path`, `MIM_PLUGIN_PATH`, the `mim` directory next to `libmim`, `<libdir>/mim`.
- A plugin has two halves with the same name that must stay in sync: `<plugin>.mim` declares the public annex/axiom surface (and drives header and doc generation), while `libmim_<plugin>` exports `mim_get_plugin` to register normalizers, stages, and backends.
  In-tree plugins live in `src/mim/plug/*`; plugins under `extra/*/CMakeLists.txt` are auto-discovered at configure time and their `extra/<plugin>/lit/*.mim` tests are staged into `lit`.
  Plugin names may only use letters, digits, and underscores, and are limited to 8 characters.
- Optimization is phase-driven.
  `optimize(World&)` looks for an entry point (`_compile`, `_default_compile`, or any nullary external returning `%compile.Phase`), resolves stages from the plugin registry, and runs a `Phase`/`RWPhase`/`PhaseMan` pipeline; without an entry point optimization is skipped.
  `RWPhase` rebuilds the old world into a new inherited world and swaps them at the end; `Analysis` and `PhaseMan` provide the fixed-point machinery.
  Use these instead of ad hoc whole-program traversals — the old `Pass`/`PassMan` machinery is gone.
- `src/automaton/` is a separate static library backing the regex subsystem.
- Tests come in two layers: `lit/` drives the CLI end-to-end with `RUN:` lines plus `FileCheck`, and `test/*.cpp` exercises library APIs with [doctest](https://github.com/doctest/doctest) (`mim-test`, `mim-regex-test`).

## Core IR invariants

- Keep the immutable/mutable `Def` split: non-binders are hash-consed and normalized on construction, whereas mutables are created first and filled in later to support recursion and variables.
- Terms and types live in the same `World` graph, so type-level computation goes through the same normalization and sharing as ordinary terms.
- Annexes and external mutables are the world roots that analyses and `RWPhase` traverse; `Cleanup` removes whatever is unreachable from them.

## Comments

Comments are scarce. The default is **no comment**.

Comment only when the code itself cannot reasonably express the information.

- Comment **why**, not what the code does.
- Prefer a better name, structure, or API over a comment.
- Keep comments to **one short sentence**, normally one line.
- When a comment spans multiple lines, use **one complete sentence per line**. Do not wrap a single sentence across multiple lines merely to fit a line-length limit.
- A comment should convey one fact only: an invariant, non-obvious constraint, algorithmic reason, or important external reference.
- Do not explain the implementation, summarize a function, or provide a narrative of its control flow.
- Match the comment density and brevity of the surrounding code. **Never increase comment density.**
- Do not add documentation-style prose, introductions, conclusions, or motivational/explanatory language.
- Do not use rhetorical contrasts such as `"X" -> "Y"`, `"instead of X"`, or `"from X to Y"` to explain an optimization.
- Do not add comments describing the change itself ("now handles X", "renamed from Y"); that belongs in the commit message.
- Do not add banner or section-header comments.
- Do not add a comment if deleting it would leave the code equally correct and understandable.
- A comment that restates the code is worse than no comment:
  ```cpp
  vec.push_back(x); // BAD: "put x into the vector"
  ```

**Hard limit:** Do not write multi-line comments unless the user explicitly asks for documentation or the comment is required to document a non-obvious invariant that cannot be stated briefly.

Before adding a comment, ask:
1. Is this information necessary?
2. Is it already apparent from the code or names?
3. Can it be expressed in one short sentence?
If the answer to 1 or 3 is no, do not add the comment.

## Style

Language-specific rules live in `.github/instructions/*.instructions.md` (C++, CMake, Mim).
