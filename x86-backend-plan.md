# Native x86-64 Backend

## Goal

An SSA-based native x86-64 backend for MimIR.
Instruction selection and register allocation both use the PBQP solver in `src/pbqp` (`libpbqp`).
SSA destruction happens only very late, during register allocation (Hack style).
Everything stays in MimIR, libFirm style: no separate machine IR.

## Decisions

### Everything stays in MimIR

- Isel, scheduling, spilling, register allocation, and SSA destruction are all phases on the World.
  Isel changes types and is an `RWPhase`; scheduling and spilling rewrite each function in place, and spilling creates a new Lam only for a block whose signature changes.
- Dumping, `-o` round-trip, lit tests, and the type checker work at every backend stage.
- The schedule becomes an explicit edge in the graph (libFirm keeps it as a side list in the node info instead, which would not survive `-o`).

### Instruction types are plain integers

- x86 instructions are axioms typed over `Idx s`, e.g. `x86.add: {s: Nat} → «2; Idx s» → Idx (gpr s)`.
- Registers and the schedule are no concern of these types.
- Constants are explicit: a literal operand of an instruction is an immediate and must fit its encoding (a sign-extended `i32`, any shift count), and `x86.mov c` (`mov r, imm`) puts any other constant into a register.
  Isel decides this per use, as part of the cover (Buchwald & Zwinkau, *Instruction Selection by Graph Transformation*, CASES 2010: `Add+Const` versus atomic `Const`).
  A literal is no PBQP node: it is hash-consed across the whole world and free to rematerialize, so each using option prices it locally (immediate 0, `mov` 1) and scheduling rematerializes a closed instruction right before each use.
- An address is an operand of type `x86.Addr i`, built by `x86.addr (disp, base)` and `x86.index a (index, scale)`; it is no value, so it is not scheduled and occupies no register of its own.
  Isel folds address arithmetic into it; RA may still unfold it into `x86.lea` under pressure.
- `x86.lea: Addr i → I64` materializes an address that is used as a value; `mem.Ptr` becomes `I64` during isel.

### The schedule is the `mem.M 0` chain

- Scheduling makes the `mem.M 0` chain total over all instructions; x86 has one address space, so isel collapses `mem.M a` to `mem.M 0`.
- Memory instructions already sit on the chain; pure ones join it via `x86.sched: {D C: *} → (D → C) → mem.M 0 → D → [mem.M 0, C]`, e.g. `let (m, x) = x86.sched x86.add m (a, b)`.
- After scheduling, no x86 instruction is applied outside `sched` or the chain; `x86.schedule` rejects an instruction it did not place.
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

## Status

Done (on `feature/x86`):

- `x86` plugin (`src/mim/plug/x86/`), with `mem.add_mem` (if `x86` is loaded), `x86.isel`, `x86.schedule`, `x86.spilling`, and `x86.emit` at the end of the default pipeline in `opt.mim`; by then no axiom pins a continuation's type, so every continuation has its `mem.M`.
- `x86.mim`: `Fl`, `Addr` (`Base`, `Index`), `Spill`, `addr`, `scale_1`/`_2`/`_4`/`_8`, `index`, `lea`, `mov`, `rm`, `mr`, `load`, `store`, `add`, `sub`, `and`, `or`, `xor`, `imul`, `shl`, `shr`, `sar`, `movzx`, `movsx`, `trunc`, `cmp`, `test`, `cc`, `cmovcc`, `spill`, `reload`, plus the size predicates.
- `x86.isel` (`src/mim/plug/x86/phase/isel.cpp`) covers `core.wrap`, `core.bit2` (and/or/xor), `core.shr`, `core.icmp`, `core.conv`, `mem.load`/`mem.store` of integers and pointers, `mem.lea` (array index or constant struct offset), `mem.assume`, `mem.mslot`, `core.idx`, `core.idx2nat`, `core.rem`, `core.div`, `core.nat`, `core.ncmp`, integer `core.bitcast`, and a select `(x, y)#c` (as `cmovcc`).
  An `Idx n` of dynamic size and `Nat` become `I64`, as in `ll`; `Nat` literals stay `Nat` so they still work as type-level arguments, and become `I64` only as values.
  The loop hints `ll.vec`/`ll.novec` are dropped.
  Any other `core`/`mem`/`math` operation left in the selected world is an error naming the axiom.
- `x86.schedule` (`src/mim/plug/x86/phase/schedule.cpp`) is a `NestPhase` that rewrites each function in place (it changes no types): it places every instruction of a function once with `Scheduler::smart` (as late as possible, but out of loops) and threads each continuation's instructions onto its `mem.M 0` chain in topological order.
  Memory instructions are rethreaded, existing `sched`s re-placed (so the phase is idempotent), all others wrapped in `x86.sched`; every other use of a memory token gets the end of the chain.
  A closed instruction has no place of its own (`Scheduler::is_remat`): it gets a fresh copy right before each use.
  So do `cmp`/`test` and `cc`, since flags are no register value: each consumer recomputes them right before itself, and a terminator places its condition last, so at most one flags value is live and nothing clobbers it before its consumer.
  A continuation without a `mem.M` is an error.
- `Liveness` (`src/mim/plug/x86/liveness.cpp`) is a side table over one scheduled function: blocks with their chains, predecessors and successors, live-in and live-out sets (path exploration per use), and MaxLive per register class (general-purpose, flags; spill slots are class `Stack` and unbounded).
  A value is a non-memory result of a chain instruction or a non-memory parameter; a parameter is defined at block entry and a jump uses its arguments, so a parameter is never live-in.
  A branch on an `x86.cc` uses its flags; the `Bool` is a value only if something else uses it.
  `x86.live` prints it (`-X x86:live=<file>`) and is not part of the default pipeline.
- `x86.spilling` (`src/mim/plug/x86/phase/spilling.cpp`) brings MaxLive of the general-purpose class down to `k` (14 by default, `-X x86:k=<n>`), per function and in place (Braun & Hack, CC 2009):
  - Global next-use distances by a fixpoint over the CFG, plus `2^20` across an edge into a smaller loop depth; loops are natural loops over the dominator tree (Cooper, Harvey, Kennedy) of the block graph, since `Nest` misses closed blocks.
  - `W_entry`: at a block whose predecessors are all processed, the candidates in a register at the end of all predecessors first, then those of some, by next use; otherwise (loop header) the candidates used in the loop first, then live-through ones only as far as the loop's MaxLive leaves room.
  - Per instruction: reload missing operands, `limit(W, k − extra)` (`div`/`idiv` need `rdx`), `limit(W, k − #defs)` after it; a call keeps at most `k − 9` values across it (five callee-saved registers at `k = 14`).
  - A parameter of the entry, a return continuation, or a branch target, and a dead parameter, is always in a register; any other parameter outside `W_entry` becomes a memory φ of type `x86.Spill s`.
  - A value is spilled right after its definition (or at its block's entry), and only if it is reloaded or flows into a memory φ; a literal flowing into a memory φ is spilled at the end of the predecessor.
  - A closed instruction is recomputed instead of reloaded.
  - Coupling reloads go to the end of the predecessor.
    EtaConv does not quite rule out critical edges: a hash-consed branch tuple can share one wrapper among several branches, so `x86.spilling` first gives each branch to such a target a fresh wrapper.
  - Reloads give a value several definitions; SSA reconstruction (Braun et al., CC 2013, every block sealed, union-find for trivial φ removal) runs symbolically first, so each block whose signature changes is rebuilt as a new Lam exactly once with its memory φs and new φ parameters appended.
  - `spill`/`reload` are pure instructions on the chain via `sched`; `x86.emit` treats them as copies between stack slots.
- `x86.load`/`x86.store` work on `mem.M 0`; isel collapses `mem.M a` to `mem.M 0`.
- `x86.emit` (`src/mim/plug/x86/emit.cpp`) writes GAS text in AT&T syntax to `<world>.s` (`-X x86:o=<file>`):
  it reads each block's order off the `mem.M` chain backwards from its terminator, keeps every value in its own stack slot with `rax`/`rcx` as scratch, takes up to six SysV integer arguments, and returns in `rax`.
  A block is a label in the dominator-respecting order of discovery; a jump stores its arguments into the target's parameter slots (through temporaries if it passes the target's own parameters) and a two-way branch on an `x86.cc` becomes `j{cc}`.
  A call loads the argument registers, stores `rax` into the return continuation's parameter slot, and jumps there; a call through the function's own return continuation is a tail of `call`, `leave`, `ret`.
  A non-external callee is emitted as a local function `<name>_<gid>`; an external declaration is called by its symbol.
  `cmp`/`test` are recomputed right before each `cc`/`cmovcc` and `jcc` (now redundant with `x86.schedule`, which already places a fresh copy there).
  This allocation is a throwaway stand-in inside `emit`; the real allocator will answer "where does `v` live" in the graph.
- `x86.cmovcc cc (f, x, y)` yields `y` if `cc` holds, like `(x, y)#cond`.
- `x86.isel` follows *Instruction Selection with PBQP*: a `Rule` is one object (name, type of its replacement, builder), and one `match` function appends a node's matches as `Alt`s with typed `Demand`s on the operands and the inner nodes they cover (`Cover`).
  `ISel::analyze()` builds the PBQP generically from that: every operation with a match is a node, inner alternatives are derived from the covers, edges price `conv` between rule type and hole type, and all-zero edges are left out.
  The rewrite is generic too: a root alternative's rule builds the replacement, `hole` converts operands to the hole's type, and an inner node defers to the root of its match.
  The first alternative of every node is atomic (asserted, as is that the atomic cover is valid), and it is the fallback should RN end infeasible.
- Rules: `lea_off_base`/`lea_off_index` (polymorphic in the index), `lea_index` (onto a `Base`), `load`, `store` (register or immediate), `bin` (`core.wrap`/`core.bit2`/`core.shr`, register or immediate), `bin_rm`, `store_mr`, `cmp`, `nat`, `unary` (conversions), `div`, `slot`, `assume`, `select`.
- Not yet as designed: matchers are C++ code with their conditions inline, so atomic completeness and composability are asserted per program instead of checked over the rule set; this comes with rules as data.
- `x86.emit` prints a literal operand as `$c` and rejects one where the instruction needs a register.
- Address rules: constant indices and struct offsets go into `disp`, one dynamic index with an element size of 1, 2, 4, or 8 into `index`/`scale` (otherwise `imul`), and a second dynamic index or a `disp` beyond `i32` materializes the prefix with `lea` or `add`.
- `bin_rm` folds a sole load into `add`/`sub`/`and`/`or`/`xor`/`imul` as `x86.rm f (m, a, addr)` (`op r, addr`): the user moves to the load's place in the memory order, so its other operand must be in scope of the load's block and must not depend on the load.
  `store_mr` folds a store right behind the load (the load's memory has no other user) of an operation on the loaded value to the same address into `x86.mr f (m, addr, b)` (`op addr, r/imm`; not `imul`).
  `cmp`/`test` do not fold since emit rematerializes them per consumer, and `div` does not since `cltd` clobbers the index register.
- `x86.div`/`idiv` yield quotient and remainder as a pair, so a quotient and a remainder of the same operands share one instruction; `x86.slot n` reserves `n` frame bytes on the `mem.M` chain; `trunc` to a non-power-of-two size is an unsigned remainder.
- `isel` widens 8-bit and `Bool` multiplications to `i32` (`movzx`, `imul`, `trunc`), since `imul` has no 8-bit form.
- Tests: `lit/x86/types.mim`, `lit/x86/isel.mim` (with `-o` round-trip), `lit/x86/isel_imul8.mim`, `lit/x86/emit_run.mim`, `lit/x86/emit_cf.mim` (loop with a parameter swap, recursion), `lit/x86/emit_addr.mim` (folded address modes), `lit/x86/emit_rmw.mim` (`rm`/`mr`), `lit/x86/emit_pbqp.mim` (competing folds), `lit/x86/sched.mim`, `lit/x86/sched_join.mim` (one placement in the dominator), `lit/x86/sched_loop.mim` (hoisting), `lit/x86/sched_remat.mim`, `lit/x86/sched_closed_tuple.mim`, `lit/x86/sched_no_mem.mim`, `lit/x86/sched_no_chain.mim`, `lit/x86/live_loop.mim`, `lit/x86/live_call.mim`, `lit/x86/size_imul8.mim`, `lit/x86/size_add5.mim`, `lit/x86/spill_loop.mim` (live-through values spilled outside the loop), `lit/x86/spill_phi.mim` (memory φ), `lit/x86/spill_ssa.mim` (new φ for a reloaded value), `lit/x86/spill_call.mim` (callee-saved limit); `lit/x86/emit_cf.mim` also runs at `k = 3`.

Known gaps:

- Placement and order ignore register pressure: hoisting lengthens live ranges, the order within a block is plain topological, and nothing keeps a flag-clobbering instruction out of a `cmp` … `jcc`/`setcc` window.
- A closed instruction below a shared non-instruction Def (a tuple, an `x86.addr`) is rematerialized once per continuation only, and a copy can end up unused on the chain.
- `mem.add_mem` places a memory operation with `Scheduler::late`, so sibling continuations share it; a closed one (a placeholder memory and constant operands) has no registered uses and still goes to its first user only.
  Run before `affine.For` is lowered, a continuation that an unpinned caller passes to a pinned function gains a `mem.M` the callee's type does not have.
- Rematerializing flags per consumer keeps the operands of a `cmp`/`test` live up to its last consumer.
  Two `cmovcc`s that differ only in the condition code share their operand tuple, and with it one copy of the `cmp`, so flags can still be live across another instruction there (the same memoization as for closed instructions).
- EtaConv does not rule out critical edges: a hash-consed branch tuple used by several branches shares its η-wrappers, so such a wrapper has several predecessors (`lit/regex/match_mail.mim`).
  Rewriting the shared tuple is memoized, so EtaConv would have to rebuild branch tuples per App.
  Until then, `x86.spilling` splits these edges itself; any other phase that assumes no critical edges has to do the same.
- `x86.emit` does not use `Liveness` or the spill decisions yet; it still gives every value its own slot.
- Spill placement is "right after the definition", so a value defined in a loop and reloaded after it is spilled on every iteration; no spill-slot coloring or memory-φ coalescing yet (each `Spill` value is a slot of its own in emit).
- Rematerialization covers closed instructions only; a `reload` is never folded into a memory operand (`x86.rm`).
- The `extra` registers of an instruction are only modeled for `div`/`idiv`; a `trunc` to a non-power-of-two size (a `div` in emit) and shifts by `cl` are not.
- Calls are modeled only as "at most `k − 9` values live across"; all arguments must be in registers at the call, and nothing reloads an argument directly into its argument register.
- More register arguments or operands than `k` at a function entry, a return continuation, a call, or an instruction is an error, not a fallback: at `k < 6`, a function or call with six arguments fails.
- Every parameter of a branch target stays in a register, since both targets share one argument tuple; so the branch forces all its arguments into registers.
- A dead parameter takes a register at its block's entry; if its argument is not in a register, the jump passes a literal `0`.
- A literal flowing into a memory φ is spilled as an immediate store; a 64-bit literal beyond `i32` needs a `mov` first, which nothing inserts yet.
- Loop exits are detected by comparing loop depths, so an edge between two sibling loops of equal depth gets no penalty.
- An irreducible block is initialized like a loop header without a loop body, so it admits live-through values up to `k`.
- Admitting live-through values at a loop header uses the loop's MaxLive before spilling, which can still force a reload at the end of the latch.
- The spiller computes its own dominators and natural loops because `Nest` misses closed blocks; these should eventually come from one place.
- Efficiency: one `Rewriter` per instruction, a linear scan per eviction, and loop bodies as vectors with linear membership tests.
- Re-running `x86.spilling` on its own output is untested.
- Only `emit_cf.mim` runs at a small `k` in lit; the sweep of the whole allowlist at `k = 3` and `k = 6` was manual (lowering the default), so it needs a lit parameter or a CI job to stay checked.
- Re-running the full pipeline on backend output is not meaningful (`core.legalize_idx` η-wraps `x86.trunc` to `Idx 4`); round-trip backend dumps with `--no-opt`.
- `isel` still emits size-check failures for non-register sizes (e.g. `core.wrap.add` on `Idx 5`); these need masking or a clear rejection.
- `isel` emits a `trunc` → `movzx` pair for array indices that `core.legalize_idx` narrowed; it is a mask (`and x, 2^k - 1`), not a no-op, and should become one `and` in PBQP isel.
- `malloc`/`free`, aggregate loads and stores, `bitcast` of unions or floats, `core.mulrem`, and `math` are not selected; `core.nat.div`/`rem` do not guard against zero like `ll`.
- `x86.emit` handles neither switches with more than two targets, indirect calls, more than six arguments, nor `movsx`/`trunc` to non-power-of-two `Idx` sizes; it does not prefix symbols for Mach-O.
- `x86.emit` jumps to the next block even when it would fall through, and leaves the dead `setcc` of a branch condition in place.
- Loading `plugin x86` in a source file pulls `x86.emit` into the default pipeline; tests of isel or scheduling alone use a local `_compile` with just those phases.
- A user-file `axm` with a checked codomain dumped as `… → Idx _1` with `_1` unbound; likely a dumper round-trip bug.

## Next Steps

1. **PBQP instruction selection**: rules are single objects with typed holes now (see *Staging*); next is rules as data with the solvability checks over the rule set, and only then rules in bulk (`cc` for flags consumed by branches and `cmov`, scaled indices from `shl`/`mul`, three-operand `add` via `lea`, the `and` mask for `trunc` → `movzx`).
2. **Register allocation**: spilling is done; next is PBQP coloring of the spilled program, then SSA destruction, then pressure-aware list scheduling; each piece replaces a stand-in and is checked against the allowlist, also at small `k`.

## Instruction Selection with PBQP

The base is Buchwald & Zwinkau, *Instruction Selection by Graph Transformation* (CASES 2010): selection is a cover of the program graph by matches of rewrite rules, found as a PBQP.
The one generalization is that the holes of a pattern are typed MimIR terms instead of always meaning "a value in a register".
This keeps their rule shape and solvability argument and adds the factoring of a grammar (Ebner et al., LCTES 2008).

### Rules

- A rule is `pattern ↦ replacement` plus a cost.
  Both sides are Mim terms over the same holes: the pattern over `core`/`mem`, the replacement over `x86`.
- A hole is a variable whose type is an x86 operand type: `Idx s` (a register), `Addr i`, or `Fl`.
- `Addr: Bool → *` states in its type whether the address has an index, with the aliases `Base = Addr ff` and `Index = Addr tt`: `x86.addr (disp, base): Base` and `x86.index a (index, scale): Index` for `a: Base`.
  So "at most one index" is a typing fact: the dynamic-index rule has a hole of type `Base`, the constant-offset rule is polymorphic in `i`, and a second index converts `Index` to `I64` (`x86.lea`) and on to `Base`.
- The `mem.M` token is order only: it is neither a hole nor a result of a pattern and no PBQP edge; the replacement rewires it.
- An instruction with several results is tuple-valued, with one single-root rule per result: `udiv (a, b) ↦ (x86.div (a, b))#0` and `urem (a, b) ↦ (x86.div (a, b))#1`.
  Hash-consing merges the replacements into one instruction; the paper's patterns have one root, and this needs neither proxy nodes nor rules with two roots.
  The solver prices such an instruction once per result, which is harmless as long as no cheaper alternative competes.
  The type of a rule is the type of its replacement.
- A literal in a pattern is a pattern node, not a hole, like `Const` in the paper's `Add+Const`; it carries a range condition.
  Literals are no PBQP nodes (see *Instruction types are plain integers*), so they may overlap freely, and a register hole that lands on one costs an `x86.mov`.
- Operand forms are rules of their own, written once: `mem.lea (p: Base, i)` with a matching element size `↦ x86.index p (i, scale)` has type `Index`.
  Instruction rules only mention the hole: `mem.load (m, p: Addr i) ↦ x86.load (m, p)`.
  So the rule count is instructions plus operand forms, and since an address rule may itself have an `Addr` hole, `mem.lea` chains of any length fold.
- A multi-node instruction is a graph pattern with typed inner nodes, as in the paper: `add (a, (mem.load (m, p: Addr i))#1) ↦ x86.rm x86.add (m, a, p)`.
- A conversion is a rule whose pattern is a single hole: `a: Addr i ↦ x86.lea a`, `c: literal ↦ x86.mov c`.

### PBQP construction

- Every operation of the program is a node, also those with only one rule.
  Vars and literals are no nodes; a Var is a register.
- The alternatives of a node `v` are, as in the paper, the equivalence classes of the matches covering `v`:
  one *root* alternative per rule matching with its root at `v`, and one *inner* alternative per class of matches that cover `v` with an inner pattern node.
- The cost of a rule sits on its root alternative; inner alternatives cost nothing.
- An edge from a user `u` to its operand `v` has, for an alternative of `u` that stems from the match `(P, ι)`, the entry (generalizing the paper's Eq. 5):
  - `P` has a hole of type `T` at `v`: the alternative of `v` must be a root alternative of some type `T'`; the entry is the cost of converting `T'` to `T`, or ∞ if there is no conversion.
  - `P` has an inner node at `v`: the alternative of `v` must be the equivalent inner one; the entry is 0, else ∞.
- Conversions live on edges, so each user converts for itself: one load folds an address that a call takes as a value.
  The conversion closure between types is computed once.
  A conversion is priced once per converting user although hash-consing materializes it only once.
- An operand that is no node adds its cost to the alternative's vector entry.

### Side conditions

Conditions come from a small fixed vocabulary instead of arbitrary predicates per rule (the paper's negative application conditions):

- *sole*: an inner node's value has no user outside the match, so the match does not duplicate it; required for loads.
- *range*: a literal fits its encoding.
- *size*: the operand size is one the instruction supports; this is the `refly.check` in the instruction's codomain.
- *memory order*: nothing may lie between the memory operations a match covers; the matcher rejects such a match before the PBQP is built.
- *availability*: if the replacement sits at an inner node's place in the memory order (`x86.rm` sits at its load), the other holes must be available there.

### Solvability

- *Atomic completeness*: every operation has a rule whose holes are all registers and whose type is a register.
- *Total conversions*: every type a rule may have converts to a register (`Addr` via `x86.lea`, a literal via `x86.mov`, flags via `setcc`).
- *Composability* (the paper's Def. 12) is checked for the multi-node patterns only: cutting any successor of the root and the spanning graph of any inner node must again be covered by rules.
  A cut at a typed hole is a rule boundary already.
- All three are checked mechanically over the rule set; a missing rule is reported by name.
- The solver deletes forbidden options and propagates the deletion, which is the paper's infinity propagation.
- The paper's proof covers untyped holes only; typed holes are a factored presentation of such a rule set, but recursive operand rules make that expansion infinite.
  So selection keeps the fallback: the atomic rules everywhere are always a valid cover.

### Replacement

- Selection runs in `ISel::analyze()`; the rewrite then applies the chosen alternatives.
- A root alternative builds its replacement with each hole substituted by the rewritten operand, converted where the edge said so.
- A node with an inner alternative is not rewritten on its own; its match's root builds it.

### Staging

- Now, each rule is one C++ object: pattern matcher, hole types, conditions, cost, and replacement builder.
  The PBQP construction and the rewrite are generic over the rule list; the solvability checks are still asserted per program.
- Later, the rule list is generated from Mim rules (*Pattern Database*); nothing else changes.

### Open points

- A multi-result pattern that should win only because it is shared (`sub` and `cmp` of the same operands) is underpriced by the one-rule-per-result scheme; to be revisited with flags.
- Flags: `cmp` + `cc` as a rule of type `Fl` consumed by branches and `cmovcc`; keeping the flags alive until then is the scheduler's job.
- The cost unit: µops or latency from uops.info for a chosen µarch, with encoding size as tie-breaker.

## Register Allocation

SSA-based allocation (Hack/Braun) with PBQP coloring, each step a phase on the World:

- Pressure-aware list scheduling.
- Spill first until MaxLive ≤ k (Braun & Hack, *Register Spilling and Live-Range Splitting for SSA-Form Programs*, CC 2009): done as `x86.spilling`, see *Status*.
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
  The allowlist is a set of `%if x86_64-linux %{ … %}` RUN lines in those tests (feature set in `lit/lit.cfg.py`): every test that `-p opt … -p x86` compiles and runs correctly, 50 in total.
  The others need calls to `malloc`/`free`, aggregate memory operations, unions, or `math`.
- Unit tests (`mim-x86-test`): register-allocation invariants, and `solve` against `solve_exhaustive` on small instances.
- The optimizer-dump oracle for any change to the pre-isel pipeline.
