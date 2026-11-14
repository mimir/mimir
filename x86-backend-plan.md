# Native x86-64 Backend

## Goal

An SSA-based native x86-64 backend for MimIR.
Instruction selection and register allocation both use the PBQP solver from `~/projects/pbqp`.
SSA destruction happens only very late, during register allocation (Hack style).
Everything stays in MimIR, libFirm style: no separate machine IR.

## Decisions

### Everything stays in MimIR

- Isel, scheduling, spilling, register allocation, and SSA destruction are all `RWPhase`s on the World.
- Dumping, `-o` round-trip, lit tests, and the type checker work at every backend stage.
- The schedule becomes an explicit edge in the graph (libFirm keeps it as a side list in the node info instead, which would not survive `-o`).

### Instruction types are plain integers

- x86 instructions are axioms typed over `Idx s`, e.g. `x86.add: {s: Nat} → «2; Idx s» → Idx (gpr s)`.
- Registers, immediates, addressing modes, and the schedule are no concern of these types.
- Whether an operand becomes an immediate or a memory operand (`[b + i*s + d]`) is decided right before register allocation, because it is a register-pressure question.
- `lea` is a register-allocation trick (three-address `add`/`shl` that saves a copy), so it is not an isel-level instruction either.
- `load`/`store` take the address as a plain `I64`; `mem.Ptr` becomes `I64` during isel.

### The schedule is the `mem.M 0` chain

- Scheduling makes the `mem.M 0` chain total over all instructions; x86 has one address space, so isel collapses `mem.M a` to `mem.M 0`.
- Memory instructions already sit on the chain; pure ones join it via `x86.sched: {D C: *} → (D → C) → mem.M 0 → D → [mem.M 0, C]`, e.g. `let (m, x) = x86.sched x86.add m (a, b)`.
- After scheduling, no x86 instruction is applied outside `sched` or the chain; this is a checkable invariant.
- The token makes each placement a distinct Def: copies, spills, reloads, and rematerializations at different points never merge under hash-consing.
- Every block and function takes an `M`, and every jump and call passes the current one; the backend assumes this on entry and rejects a continuation without one.
- A post-RA rescheduler would have to recompute memory dependencies; anything but loads, stores, and calls commutes with memory.

### Operand sizes are side conditions

- The supported sizes are stated with `refly.check` in the codomain, so implicit inference of `s` keeps working and a literal `s` folds the check away.
- `gpr`: `i8`, `i16`, `i32`, `i64` — `add`, `sub`, shifts, `load`.
- `gpr16`: `i16`, `i32`, `i64` — `imul`, `cmovcc` (no 8-bit forms).
- `logic`: `Bool` or a `gpr` size — `and`, `or`, `xor`.
- Not yet checked: `store`, `cmp`/`test` (their codomain does not mention `s`; `refly.check` cannot guard a type) and the conversions (`movzx`/`movsx`/`trunc`).

### Condition codes

- A condition code is a `Nat` in hardware encoding order (o = 0, …, g = 15).
- `x86.cc cc: Fl → Bool` is `setcc` as a value and `jcc` as a branch condition.
- `core.icmp` maps onto `x86.cc c (x86.cmp (a, b))` for `e`, `ne`, `sl`, `sle`, `sg`, `sge`, `ul`, `ule`, `ug`, `uge`; the other predicates are rejected for now.

### Straight-line code first

- Scheduling, register allocation, and emission first handle single-block functions only.
- Each block's chain starts at its own `M` parameter, so control flow needs no new mechanism for the schedule.

## Status

Done (on `feature/x86`):

- `x86` plugin (`src/mim/plug/x86/`), with `x86.isel`, `x86.schedule`, and `x86.emit` spliced into the default pipeline in `opt.mim`.
- `x86.mim`: `Fl`, `load`, `store`, `add`, `sub`, `and`, `or`, `xor`, `imul`, `shl`, `shr`, `sar`, `movzx`, `movsx`, `trunc`, `cmp`, `test`, `cc`, `cmovcc`, plus the size predicates.
- `x86.isel` (`src/mim/plug/x86/phase/isel.cpp`): a one-to-one translation of `core.wrap`, `core.bit2` (and/or/xor), `core.shr`, `core.icmp`, `core.conv`, `mem.load`/`mem.store` of integers and pointers, `mem.lea` (array index or constant struct offset), `mem.assume`, and `core.idx` of a literal.
  An `Idx n` of dynamic size becomes `I64`, as in `ll`.
  Any other `core`/`mem`/`math` operation left in the selected world is an error naming the axiom.
- `x86.schedule` (`src/mim/plug/x86/phase/schedule.cpp`) puts each continuation's instructions on its `mem.M 0` chain in DFS post-order: memory instructions are rethreaded, existing `sched`s re-placed (so the phase is idempotent), all others wrapped in `x86.sched`; every other use of a memory token gets the end of the chain.
  An instruction reachable only from nested continuations is scheduled in the first one that reaches it; scoped mappings keep siblings from sharing it.
  A continuation without a `mem.M` is an error.
- `x86.load`/`x86.store` work on `mem.M 0`; isel collapses `mem.M a` to `mem.M 0`.
- `x86.emit` (`src/mim/plug/x86/emit.cpp`) writes GAS text in AT&T syntax to `<world>.s` (`-X x86:o=<file>`):
  it reads each block's order off the `mem.M` chain backwards from its terminator, keeps every value in its own stack slot with `rax`/`rcx` as scratch, takes up to six SysV integer arguments, and returns in `rax`.
  A block is a label in the dominator-respecting order of discovery; a jump stores its arguments into the target's parameter slots (through temporaries if it passes the target's own parameters) and a two-way branch on an `x86.cc` becomes `j{cc}`.
  A call loads the argument registers, stores `rax` into the return continuation's parameter slot, and jumps there; a call through the function's own return continuation is a tail of `call`, `leave`, `ret`.
  A non-external callee is emitted as a local function `<name>_<gid>`; an external declaration is called by its symbol.
  `cmp`/`test` are rematerialized right before each `cc`/`cmovcc`, so the order of the schedule cannot clobber flags.
  This allocation is a throwaway stand-in inside `emit`; the real allocator will answer "where does `v` live" in the graph.
- `x86.cmovcc cc (f, x, y)` yields `y` if `cc` holds, like `(x, y)#cond`.
- `isel` widens 8-bit and `Bool` multiplications to `i32` (`movzx`, `imul`, `trunc`), since `imul` has no 8-bit form.
- Tests: `lit/x86/types.mim`, `lit/x86/isel.mim` (with `-o` round-trip), `lit/x86/isel_imul8.mim`, `lit/x86/emit_run.mim`, `lit/x86/emit_cf.mim` (loop with a parameter swap, recursion), `lit/x86/sched.mim`, `lit/x86/sched_no_mem.mim`, `lit/x86/size_imul8.mim`, `lit/x86/size_add5.mim`.

Known gaps:

- The schedule order is plain topological: no register pressure, and nothing keeps a flag-clobbering instruction out of a `cmp` … `jcc`/`setcc` window.
- `mem.add_mem` cannot provide the `mem.M` the scheduler needs: on already-threaded code it passes a stale memory to branch targets and drops a store on one path.
- Re-running the full pipeline on backend output is not meaningful (`core.legalize_idx` η-wraps `x86.trunc` to `Idx 4`); round-trip backend dumps with `--no-opt`.
- `isel` still emits size-check failures for non-register sizes (e.g. `core.wrap.add` on `Idx 5`); these need masking or a clear rejection.
- `isel` emits a `trunc` → `movzx` pair for array indices that `core.legalize_idx` narrowed; it is a mask (`and x, 2^k - 1`), not a no-op, and should become one `and` in PBQP isel.
- `malloc`/`free`, stack slots (`mem.mslot`), `core.rem`/`core.div`, `core.bitcast`, `core.idx` of a non-literal, and `math` are not selected yet.
- `x86.emit` handles neither switches with more than two targets, indirect calls, more than six arguments, nor `movsx`/`trunc` to non-power-of-two `Idx` sizes; it does not prefix symbols for Mach-O.
- `x86.emit` jumps to the next block even when it would fall through, and leaves the dead `setcc` of a branch condition in place.
- Loading `plugin x86` in a source file pulls `x86.emit` into the default pipeline; tests of isel or scheduling alone use a local `_compile` with just those phases.
- A user-file `axm` with a checked codomain dumped as `… → Idx _1` with `_1` unbound; likely a dumper round-trip bug.

## Next Steps

Done: scheduling in topological order, and emission of GAS text (AT&T syntax) with control flow and SysV calls on a spill-everything stand-in allocator (no encoder or ELF writer for now).

1. **Isel coverage** for what the sweep hits next: `core.rem`/`core.div` (`rax`/`rdx`), `mem.mslot`, `core.idx` of a non-literal, `core.bitcast`.
2. **Register allocation** with PBQP, including pressure-aware list scheduling, spilling, the pre-allocation step that picks immediates and folds address modes, and SSA destruction; each piece replaces a stand-in and is checked against the allowlist.
3. **PBQP instruction selection** replacing the one-to-one translation.

## Instruction Selection with PBQP

Following Ebner, Brandner, Scholz, Krall et al., *Generalized Instruction Selection using SSA-Graphs* (LCTES 2008):

- MimIR's hash-consed graph already is the SSA graph with explicit sharing, which is where PBQP selection beats tree tiling.
- One PBQP node per IR operation; its options are the rules whose pattern matches there, pre-filtered by structural matching (typically 2–10 options).
- Edges encode nonterminal compatibility (0/∞ plus chain-rule costs); the solver's hash-consed matrices fit, since many edges share one matrix.
- Patterns with several roots (div → quotient and remainder, op → value and flags, load folding) need Ebner's proxy nodes.
- Load folding is safe exactly when the `mem.M` chain shows no intervening store and the load has one user.
- Patterns may only cover Defs in the same block.
- Costs come from uops.info (latency/µops for a chosen µarch), with encoding size as tie-breaker.

## Register Allocation

SSA-based allocation (Hack/Braun) with PBQP coloring, each step an `RWPhase`:

- Pressure-aware list scheduling.
- Spill first until MaxLive ≤ k (Braun & Hack, *Register Spilling and Live-Range Splitting for SSA-Form Programs*, CC 2009).
- PBQP coloring with `rn_priority` set to PEO positions, so RN never fails on chordal SSA graphs (Buchwald, Zwinkau, Bersch, *SSA-based Register Allocation with PBQP*, CC 2011); `pbqp::solve` already supports this.
- Fixed-register instructions (`div` with rax/rdx, shifts by `cl`, calls, `ret`) get a parallel copy right before them, so the graph stays chordal.
- Two-address forms: if operand 0 is still live after the instruction, insert a copy first, then add a tied edge between result and operand.
- φ copies become negative-cost affinity edges in the same PBQP instead of a separate coalescer.
- Subregisters (al/ax/eax/rax): one option set per class, aliasing constraints in the matrices.
- SSA destruction at the end: block params stay as the φs; insert parallel copies before jumps so argument and parameter registers agree, then sequentialize them (`xchg` for cycles, else a scratch register).

Copies, spills, and reloads at different program points stay distinct Defs because each takes its chain token (see *The schedule is the `mem.M 0` chain*).

## Pattern Database

Don't scrape patterns; factor the database as semantics × forms × costs.
The explosion is in forms (widths, register/memory/immediate operands, VEX/EVEX), not in meaning.

- **Catalog (generated)** from uops.info `instructions.xml`: XED iform/iclass, all operands including implicit ones with width and read/write, per-flag read/write, ISA extension, and per-µarch latency/throughput/ports.
  XED's datafiles are the Apache-2.0 alternative; check the uops.info data license before vendoring.
- **Semantics (hand-written, small)**: one Mim `lam` per iclass, written over `core`/`math`/`mem` and parametric in width.
  About 100–150 iclasses cover what a compiler emits.
- **The pattern database is these semantic terms read backwards**: isel compiles them into a rule table.
  The same source also gives a reference interpreter for x86 worlds.
- **Verification**: differential tests per generated form, running the real instruction against the normalized Mim semantics on random inputs.
- **LLVM TableGen as oracle, not source**: `llvm-tblgen X86.td --dump-json` gives a JSON dump for a coverage report and for cross-checking tied operands and implicit defs.
  As a primary source it is weak: semantics hide behind `X86ISD::*` nodes and C++ predicates, and EFLAGS is opaque.
- Rejected as primary sources: Ghidra SLEIGH (decoder-oriented), K-x86-64 (huge, K-specific), Strata (partial).
- Long-term option: synthesize the rule library from semantics (Buchwald, Fried, Hack, *Synthesizing an Instruction Selection Rule Library from Semantic Specifications*, CGO 2018).
- Hand-written regardless: the condition-code mapping, address modes, and the ABI.

## Verification

- Integer-only lit tests that run under `-p ll` should also run under `-p x86` with the same expected output.
  The allowlist is a set of `%if x86_64-linux %{ … %}` RUN lines in those tests (feature set in `lit/lit.cfg.py`); currently `ret_argc`, `fun_main_argv`, `affine/affine_index`, `affine/affine_index_op_mul`, `mem/alloc_load_store`, `mem/slot_load_store`, `mem/two_fun_mem`, `mem/two_fun_no_mem`, `core/ret_add`, `core/ret_and`, `core/ret_lshr`, `cps/ds_external_cimp`, `cps/ds_external_cimp_translated`, `main_loop`, `mem/seo_known_to_unknown`.
  Of the other `clang`-run tests, five have a continuation without `mem.M` (`affine/for_*`, `affine/dynamic_for`, `core/pow`); the rest stop in isel, mostly at `core.idx` of a non-literal (the regex tests), `core.rem`, and `mem.mslot`.
- Unit tests (`mim-x86-test`): register-allocation invariants, and `solve` against `solve_exhaustive` on small instances.
- The optimizer-dump oracle for any change to the pre-isel pipeline.
