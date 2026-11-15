# libpbqp

A PBQP (Partitioned Boolean Quadratic Programming) solver for the x86 backend's instruction selection and register allocation.
It is a static library like `src/automaton/` and depends only on `fe`; it knows nothing about MimIR.
The repo-wide rules in the root `AGENTS.md` apply, including the comment rules.

## Build & test

| Task | Command |
| ---- | ------- |
| Build | `cmake --build build --target pbqp` |
| Tests | `build/bin/mim-pbqp-test` (`-tc=NAME` runs one `TEST_CASE`), from `test/pbqp.cpp` with inputs in `test/data/pbqp/` |

The library is kept warning-free under `-Wpedantic -Wconversion -Wshadow` (`-Wshadow=local` on GCC) on top of the repo's `-Wall -Wextra`.

## Design

- `include/pbqp/` holds the public headers and `src/pbqp/` the implementations; `cost.h` is private.
- `fe` is a public dependency: the public headers use `fe::Vector` for storage and `fe::View`/`fe::Span` for spans.
- `Graph` only grows: it stores cost vectors and matrices under dense ids handed out from 0, with at most one edge per node pair; `add_edge_costs` sums onto an existing edge in either direction.
- Storage stays private (read-only spans, `edge(e)` endpoints, `set_node_costs`/`set_edge_costs`), so equal edge matrices are hash-consed into one copy in an append-only `ankerl::unordered_dense` set whose positions are the matrix ids; a matrix that `set_edge_costs`/`add_edge_costs` replaces stays in the pool.
- `Cost` is `std::int32_t` and `CostSum` totals are `std::int64_t`; `kInfinity` marks forbidden entries and makes any total `kInfiniteSum`.
- `solve` reduces a normalized `int32` working copy (non-negative, saturating at `kInfinity`, all-zero edges dropped) whose edges borrow the graph's matrices until their first write, which copies into an `fe::Arena` as plain arrays.
- Transient matrices live in a second, rolled-back scratch arena; RII/RM results are allocated last so they can become the edge's matrix or be rolled back.
- Worklists separate exact reductions from RN candidates: forbidden options are deleted (tracked back to original indices), single-option nodes and RI/RII are reduced exactly, and RM merges implied neighbours right before RN.
- RN commits to the locally best option, taking the highest optional `rn_priority` first (PEO positions for SSA-based register allocation), then the highest degree, then the widest cost spread.
- `solve` back-substitutes in reverse reduction order; infeasible solutions carry `conflicts`, the nodes whose choice hits a `kInfinity` entry.
- `solve_exhaustive` is the reference for tiny instances.
- `read_json`/`write_json` speak our own JSON format: a node's position in `nodes` is its id, edge `costs` are row-major with `src` as the row (`rows`/`cols` are optional checks), `"inf"` is `kInfinity`, and the optional `rn_priority` has one entry per node.
  The reader is built on `fe::Lexer`/`fe::Parser`, keeps string escapes verbatim, recovers at anchored `}`/`]`, and throws everything it collected once parsing is done.
- Reduction kernels are plain 32-bit add/min loops that the compiler vectorizes; on x86 ELF, `target_clones` adds x86-64-v4/AVX2 runtime dispatch (off under ThreadSanitizer).
