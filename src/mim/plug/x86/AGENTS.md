# Native x86-64 Backend

This is the design and the living status of the `x86` plugin; update *Status* and *Known gaps* with every backend change.
Never propose a separate machine IR: everything stays in the World (see *Decisions*).

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
- For two-address instructions the tied value operand comes first; the tie is only a register-assignment constraint (`x86.add (a, b)` computes `a + b`), and `x86.emit` reverses the value operands for its AT&T output.
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

- `x86.cc` and `x86.cmovcc` are families in hardware encoding order (`x86.cc.o` = 0, …, `x86.cc.g` = 15), so a code is a subtag rather than an operand: C++ compares `Axm::base()` and reads `Axm::sub()`.
- `x86.cc.l: Flags → Bool` is `setcc` as a value and `jcc` as a branch condition.
- `core.icmp` maps onto `x86.cc.<c> (x86.cmp (a, b))` for `e`, `ne`, `sl`, `sle`, `sg`, `sge`, `ul`, `ule`, `ug`, `uge`, one `patterns_` row per predicate with its condition code as the `Insn`; the other predicates have no row and are rejected for now.

## Status

Done (on `feature/x86`):

- `x86` plugin (`src/mim/plug/x86/`), with `mem.add_mem` (if `x86` is loaded), `x86.isel`, `x86.schedule`, `x86.spilling`, `x86.constrain`, `x86.color`, and `x86.emit` at the end of the default pipeline in `opt.mim`; by then no axiom pins a continuation's type, so every continuation has its `mem.M`.
- `x86.mim`: `Flags`, `Addr` (`Base`, `Index`), `Spill`, `addr`, `scale_1`/`_2`/`_4`/`_8`, `index`, `lea`, `mov`, `rm`, `mr`, `load`, `store`, `add`, `sub`, `and`, `or`, `xor`, `imul`, `shl`, `shr`, `sar`, `movzx`, `movsx`, `trunc`, `cmp`, `test`, the condition-code families `cc` and `cmovcc`, `spill`, `reload`, `in` with the module `reg` of the register lets `x86.reg.rax` … `x86.reg.r15`, plus the size predicates.
- `x86.isel` (`src/mim/plug/x86/phase/isel.cpp`) covers `core.wrap`, `core.bit2` (and/or/xor), `core.shr`, `core.icmp`, `core.conv`, `mem.load`/`mem.store` of integers and pointers, `mem.lea` (array index or constant struct offset), `mem.assume`, `mem.mslot`, `core.idx`, `core.idx2nat`, `core.rem`, `core.div`, `core.nat`, `core.ncmp`, integer `core.bitcast`, and a select `(x, y)#c` (as `cmovcc`).
  An `Idx n` of dynamic size and `Nat` become `I64`, as in `ll`; `Nat` literals stay `Nat` so they still work as type-level arguments, and become `I64` only as values.
  The loop hints `ll.vec`/`ll.novec` are dropped.
  Any other `core`/`mem`/`math` operation left in the selected world is an error naming the axiom.
- `x86.schedule` (`src/mim/plug/x86/phase/schedule.cpp`) is a `NestPhase` that rewrites each function in place (it changes no types): it places every instruction of a function once with `Scheduler::smart` (as late as possible, but out of loops) and threads each continuation's instructions onto its `mem.M 0` chain in topological order.
  Memory instructions are rethreaded, existing `sched`s re-placed (so the phase is idempotent), all others wrapped in `x86.sched`; every other use of a memory token gets the end of the chain.
  A closed instruction has no place of its own (`Scheduler::is_remat`): it gets a fresh copy right before each use.
  So do `cmp`/`test` and `cc`, since flags are no register value: each consumer recomputes them right before itself, so at most one flags value is live and nothing clobbers it before its consumer.
  Instructions that `Scheduler::smart` hoists into a block are appended after that block's own ones, i.e., after the condition of its branch; so `chain()` appends a fresh `cmp`/`cc` for the branch at the very end.
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
  - `spill`/`reload` are pure instructions on the chain via `sched`; `x86.emit` gives every `Spill` value a stack slot of its own and turns them into stores and loads.
- `x86.constrain` (`src/mim/plug/x86/phase/constrain.cpp`) runs after `x86.spilling`: right before an instruction with a fixed operand (`div`'s dividend in `rax`, a shift count in `rcx`) or a two-address operand that stays live, the block jumps to a fresh continuation that takes these operands; the fixed copies are wrapped in `x86.in`.
  Likewise, right after an instruction with fixed results (`div`'s quotient in `rax` and remainder in `rdx`), the live ones go to a fresh continuation as `x86.in rax q`, `x86.in rdx r`, so a precolored live range ends at once; this costs no extra register, as the arguments die where the parameters are defined.
  The table of fixed operands, tied operands, and fixed results is `constraint()` (`src/mim/plug/x86/constraint.cpp`), shared with the spiller and the colorer.
  `x86.spilling` counts these copies as `extra` registers; if an instruction would then need more than `k`, the operands to copy leave their registers at it instead and are reloaded at their next use.
- `x86.color` (`src/mim/plug/x86/phase/color.cpp`) runs after `x86.constrain`, per function and in place:
  - It first drops chain instructions none of whose results is used (to a fixpoint), so a result without a use is one whose register its instruction implies (`div`'s remainder); and it sets every block's filter to `ff`, since rebuilding a jump must not inline its target (pruning can make both targets of a branch α-equal, and the World then folds the branch).
  - One PBQP over the general-purpose values with 16 options; `rsp` and `rbp` are ∞, callee-saved registers cost 1.
    A dead parameter is no node.
    `-X x86:color-dot=<file>` writes each function's graph in Graphviz DOT format with the chosen options highlighted.
  - Interference edges (∞ on the diagonal) between a definition and every value live after it or defined along with it; tie edges (∞ off the diagonal) between a two-address result and its tied operand; fixed operands and results are precolored; clobbers are ∞ on the operands (`rdx` at `div`); values live across a call are ∞ on the nine caller-saved registers.
  - Copies are preferences: a jump argument and its parameter get an edge that costs 4 off the diagonal; entry parameters prefer their SysV argument registers, call arguments theirs, a return continuation's parameter and a returned value prefer `rax`.
  - `rn_priority` is the node id, and nodes are created in block discovery order (which visits a dominator first) and within a block in definition order, so RN eliminates along a PEO.
  - The rewrite wraps every use in `x86.in r v`; a whole tuple of values (a block's variable, or an instruction passed whole after η-reduction) is rebuilt from its wrapped projections.
  - A checker then recomputes `Liveness` on the result and verifies the invariants below; any violation is an error naming the values.
- `x86.load`/`x86.store` work on `mem.M 0`; isel collapses `mem.M a` to `mem.M 0`.
- `x86.emit` (`src/mim/plug/x86/emit.cpp`) writes GAS text in AT&T syntax to `<world>.s` (`-X x86:o=<file>`) on the registers of `x86.color`:
  - It walks each function's blocks with their chains from `Liveness` and reads "where does `v` live" off the `x86.in` nodes (plus the implied registers of fixed results); an operand that is no `x86.in` and no literal is an error.
  - Instructions use the registers directly; a two-address result is in the register of its tied operand, which `assert`s check.
  - A jump, a call (arguments into the SysV registers, then the result from `rax` into the return continuation's parameter), a return (into `rax`), and the entry (from the SysV registers into the parameters) are parallel copies: spill slots first via `push`/`pop` (memory φs, cycles included), then register moves in dependency order with `xchg` for cycles, then immediates; a dead parameter receives nothing.
  - Flags are emitted where `x86.schedule` placed the `cmp`/`test`; `emit` tracks which result the flags hold and rejects a consumer whose flags something clobbered in between.
  - The frame is `rbp`-based: the callee-saved registers that hold values are pushed after `rbp`, and spill slots and `x86.slot` areas follow, aligned so that `rsp` is a multiple of 16 at calls.
  - Blocks are laid out by chasing each block's last jump (a jump's target, a call's return continuation, a branch's else target or its trampoline's target) and fall through to the next block where possible.
  - A non-power-of-two `trunc` is a `div` with `rax`, `rcx`, and `rdx` saved around it by `push`/`pop`, as nothing reserves them.
  - A non-external callee is emitted as a local function `<name>_<gid>`; an external declaration is called by its symbol.
- `x86.cmovcc.l (f, x, y)` yields `y` if `l` holds, like `(x, y)#cond`.
- `x86.isel` follows *Instruction Selection with PBQP*: both sides are data, `rules_` (one entry per pattern ↦ replacement) and `patterns_` (which `core`/`mem` axiom each operation is).
  A `Rule` carries the `Op` of its root, the type of its replacement, its cost, its name, its builder, a side condition `Cond`, and its pattern as `Slot`s in preorder: each is a hole of some `Want` or an inner node (an `Op` and its arity) whose operand slots follow it.
  `Op` has one value per instruction, and one instruction is one row per operand form (`add`, `add_imm`, `add_rm`); `Op::Alu` is a wildcard inner node for any operation with a read-modify-write memory form.
  `Cond::Scale` (`lea_index`) requires an element size of 1, 2, 4, or 8; `Cond::Same`, `Cond::Narrow`, and `Cond::Widen` compare the register sizes of a conversion's operand and result and pick the identity, `trunc`, or `movzx`/`movsx`; `Cond::Fold` (`bin_rm`) requires the root's memory form and the availability of its holes at the folded load; `Cond::Rmw` (`store_mr`) requires that the folded load reads the store's address on the memory the store takes, which nothing else uses.
  A `Pattern` carries the axiom (or the member of a multi-member annex; `select_` stands for an `Extract`), its `Op`, the `Tag` that tells `decompose` how to take it apart, its `Insn`, and whether its operands commute.
  An `Insn` is the x86 instruction with its memory forms (`Mem`: `imul` has no `op m, r`), the result it takes of a pair-valued instruction (`div`), whether narrow operands are sign-extended, and whether the result is complemented (`nand` as `xor` of `and` with all ones).
  So one builder, `build_insn`, selects every operation that is one instruction (`core.wrap`, `core.bit2`, `core.shr`, `core.nat` but `sub`, `core.div`, `core.rem`); `build_id`, `build_trunc`, and `build_ext` select the conversions (`core.conv`, `core.bitcast`, `core.idx`, `core.idx2nat`), and no builder asks for the axiom again.
  A pattern row may be limited to a `mem.lea` with a constant index (`only_constant`), so `mem.lea` has one row for `Op::LeaOffset` and one for `Op::LeaIndex`.
  Which operand sizes an x86 instruction takes is read off the `refly.check` in its codomain (`supports`, memoized per instruction and size): a memory form needs it for the instruction and for `x86.load`, and `call_insn` and `build_select` go through `i32` where it fails.
  `match` decomposes a Def into its `Op` and operands and binds every rule of that `Op` slot by slot (`bind`), trying the swapped operands of a commuting node whenever its two operand subtrees differ; an inner node must be `sole` (its value and its parent's operand tuple have no other user).
  A match is an `Alt` with typed `Demand`s on the operands and the inner nodes it covers (`Cover`).
  `ISel::analyze()` builds the PBQP generically from that: every operation with a match is a node, inner alternatives are the classes of covers with equal demands (resolved bottom-up), edges price `conv` between rule type and hole type, and all-zero edges are left out.
  Conversion costs are one table `conv_` from rule type (or `Ty::Lit` for a literal operand) to hole type; `key` lets the value projection of any memory operation stand for it.
  The rewrite is generic too: a root alternative's rule builds the replacement, `hole` converts operands to the hole's type, and an inner node defers to the root of its match.
  `atomic_choice` scans the alternatives for an atomic fallback (no inner nodes, holes only `Reg`/`Base`/`Addr`) instead of trusting the table order, a `static_assert` checks that every rule's slots form a tree, and `check_rules` verifies the rule set itself (see *Solvability*).
  `-X x86:isel-dot=<file>` writes the graph in Graphviz DOT format with the chosen options highlighted.
- `Want::Sh` is a shift count: any literal, since x86 masks it, where `Want::Imm` must fit a sign-extended `i32`.
- `x86.emit` prints a literal operand as `$c` (it must fit a sign-extended `i32`) and rejects one where the instruction needs a register.
- Address rules: constant indices and struct offsets go into `disp`, one dynamic index with an element size of 1, 2, 4, or 8 into `index`/`scale` (otherwise `imul`), and a second dynamic index or a `disp` beyond `i32` materializes the prefix with `lea` or `add`.
- `bin_rm` folds a sole load into `add`/`sub`/`and`/`or`/`xor`/`imul` as `x86.rm f (m, a, addr)` (`op r, addr`): the user moves to the load's place in the memory order, so its other operand must be in scope of the load's block and must not depend on the load.
  `store_mr`/`store_mr_imm` fold a store right behind the load (the load's memory has no other user) of an operation on the loaded value to the same address into `x86.mr f (m, addr, b)` (`op addr, r/imm`; not `imul`).
  `cmp`/`test` do not fold since `x86.schedule` rematerializes them per consumer, and `div` does not since `cltd` clobbers the index register.
- `x86.div`/`idiv` yield quotient and remainder as a pair, so a quotient and a remainder of the same operands share one instruction; `x86.slot n` reserves `n` frame bytes on the `mem.M` chain; `trunc` to a non-power-of-two size is an unsigned remainder.
- `isel` widens 8-bit and `Bool` multiplications to `i32` (`movzx`, `imul`, `trunc`), since `imul` has no 8-bit form.
- Tests: `lit/x86/types.mim`, `lit/x86/isel.mim` (with `-o` round-trip), `lit/x86/isel_imul8.mim`, `lit/x86/emit_run.mim`, `lit/x86/emit_cf.mim` (loop with a parameter swap, recursion), `lit/x86/emit_addr.mim` (folded address modes), `lit/x86/emit_rmw.mim` (`rm`/`mr`), `lit/x86/emit_pbqp.mim` (competing folds), `lit/x86/sched.mim`, `lit/x86/sched_join.mim` (one placement in the dominator), `lit/x86/sched_loop.mim` (hoisting), `lit/x86/sched_remat.mim`, `lit/x86/sched_closed_tuple.mim`, `lit/x86/sched_no_mem.mim`, `lit/x86/sched_no_chain.mim`, `lit/x86/live_loop.mim`, `lit/x86/live_call.mim`, `lit/x86/size_imul8.mim`, `lit/x86/size_add5.mim`, `lit/x86/spill_loop.mim` (live-through values spilled outside the loop), `lit/x86/spill_phi.mim` (memory φ), `lit/x86/spill_ssa.mim` (new φ for a reloaded value), `lit/x86/spill_call.mim` (callee-saved limit), `lit/x86/constrain.mim` (also at `k = 3`), `lit/x86/color.mim` (fixed results, a value across a call; also at `k = 3`); `lit/x86/emit_cf.mim` also runs at `k = 3`.

Known gaps and bugs, as of 2026-11-20.

Open bugs and risks (wrong code or a crash is possible, or only a later check catches it):

- `x86.reg`, `x86.cc`, and `x86.cmovcc` have to stay in hardware encoding order, and nothing checks it: C++ reads registers and condition codes off the subtag (`encoding`, `emit`'s mnemonic table, `isel`'s `cc` ↦ `cmovcc`), so a reordering silently emits wrong registers or conditions while a register's Mim value stays as written.
- A use of a fixed-result tuple as a whole (`div`'s `insn#1` instead of its projections) bypasses `x86.constrain`'s post-split: `Subst` maps the projections to the new continuation's parameters, but `insn#1` itself to the new instruction's, so that use would see `rax`/`rdx` past the split; untested, `x86.color`'s checker would report it.
- A two-address instruction whose tied operand is a literal (`sub (5, x)`) has no tie (`constraint()` drops it), so nothing keeps the result out of the other operand's register; `emit` rejects the literal ("needs an `x86.mov`"), and isel must never produce it.
- `emit` checks two-address results only with `assert`s, so a release build relies on `x86.color`'s checker.
- PBQP coloring has no fallback: an infeasible solution is an error naming the conflicting values.
  Chordality plus the PEO priority rules this out for unconstrained values, but precolored values and the ∞ entries for clobbers and calls void that guarantee in theory.
- `emit`'s flags tracking turns a clobbered flags value into a compile error; it can still happen where two `cmovcc`s share one hash-consed `cmp` copy (the memoization noted below under *Scheduling*) and a flag-clobbering instruction lands between them.
- `prune` in `x86.color` drops a dead `load`, which removes a possible fault; volatile accesses are not modeled.
- `prune` sets every block's filter to `ff` for good, so backend dumps show η-wrappers without their `tt` filter.

Scheduling:

- Placement and order ignore register pressure: hoisting lengthens live ranges and the order within a block is plain topological.
- The branch condition copied to the end of a block (see `x86.schedule` above) leaves a dead copy behind, which only `x86.color`'s pruning removes.
- A closed instruction below a shared non-instruction Def (a tuple, an `x86.addr`) is rematerialized once per continuation only, and a copy can end up unused on the chain.
- Rematerializing flags per consumer keeps the operands of a `cmp`/`test` live up to its last consumer.
  Two `cmovcc`s that differ only in the condition code share their operand tuple, and with it one copy of the `cmp` (the same memoization as for closed instructions).
- `mem.add_mem` places a memory operation with `Scheduler::late`, so sibling continuations share it; a closed one (a placeholder memory and constant operands) has no registered uses and still goes to its first user only.
  Run before `affine.For` is lowered, a continuation that an unpinned caller passes to a pinned function gains a `mem.M` the callee's type does not have.
- EtaConv does not rule out critical edges: a hash-consed branch tuple used by several branches shares its η-wrappers, so such a wrapper has several predecessors (`lit/regex/match_mail.mim`).
  Rewriting the shared tuple is memoized, so EtaConv would have to rebuild branch tuples per App.
  Until then, `x86.spilling` splits these edges itself; any other phase that assumes no critical edges has to do the same.

Spilling:

- Spill placement is "right after the definition", so a value defined in a loop and reloaded after it is spilled on every iteration.
- Rematerialization covers closed instructions only; a `reload` is never folded into a memory operand (`x86.rm`).
- A `trunc` to a non-power-of-two size (a `div` in `emit`) has no constraint and no `extra` registers; `emit` saves `rax`, `rcx`, and `rdx` around it with `push`/`pop` instead.
- Calls are modeled only as "at most `k − 9` values live across"; all arguments must be in registers at the call, and nothing reloads an argument directly into its argument register.
- More register arguments or operands than `k` at a function entry, a return continuation, a call, or an instruction is an error, not a fallback: at `k < 6`, a function or call with six arguments fails, and at `k = 2` a `div` (three registers) fails; three allowlist programs hit the latter.
- Every parameter of a branch target stays in a register, since both targets share one argument tuple; so the branch forces all its arguments into registers.
- A dead parameter takes a register at its block's entry in the spiller's count (if its argument is not in a register, the jump passes a literal `0`), although `x86.color` gives it none and `emit` copies nothing into it.
- A literal flowing into a memory φ is spilled as an immediate store; a 64-bit literal beyond `i32` needs a `mov` first, which nothing inserts, so `emit` rejects it.
- Loop exits are detected by comparing loop depths, so an edge between two sibling loops of equal depth gets no penalty.
- An irreducible block is initialized like a loop header without a loop body, so it admits live-through values up to `k`.
- Admitting live-through values at a loop header uses the loop's MaxLive before spilling, which can still force a reload at the end of the latch.
- The spiller computes its own dominators and natural loops because `Nest` misses closed blocks; these should eventually come from one place.
- Efficiency: one `Rewriter` per instruction, a linear scan per eviction, and loop bodies as vectors with linear membership tests.
- Re-running `x86.spilling` on its own output is untested.

Coloring:

- Copy costs are not weighted by loop depth, and the callee-saved penalty is paid per value instead of once per register and function.
- An entry parameter prefers its SysV register and its jump target's parameter with the same cost, so which copy remains is arbitrary.
- Entry parameters and a return continuation's parameter are not precolored but only prefer their ABI register, so `emit` copies them from the ABI registers (a parallel copy, like a jump).
- `x86.color` builds `Liveness` three times per function (`prune`, the colorer, the checker), and the checker always runs.

Emission:

- Every `Spill` value has a stack slot of its own: no slot coloring and no memory-φ coalescing, so a memory φ costs a `push`/`pop` per jump.
- Block layout greedily chains each block's last jump; a branch whose then target takes parameters goes through a trampoline and always jumps to its else target.
- Every function sets up `rbp`, even a leaf without slots, and every call sets `eax` to 0, even for a non-variadic callee.
- `x86.emit` handles neither switches with more than two targets, indirect calls, more than six arguments, nor `movsx`/`trunc` to non-power-of-two `Idx` sizes; it does not prefix symbols for Mach-O.

Instruction selection:

- `isel` still emits size-check failures for non-register sizes (e.g. `core.wrap.add` on `Idx 5`); these need masking or a clear rejection.
- `isel` emits a `trunc` → `movzx` pair for array indices that `core.legalize_idx` narrowed; it is a mask (`and x, 2^k - 1`), not a no-op, and should become one `and` in PBQP isel.
- `malloc`/`free`, aggregate loads and stores, `bitcast` of unions or floats, `core.mulrem`, and `math` are not selected; `core.nat.div`/`rem` do not guard against zero like `ll`.

Testing and tooling:

- Only `emit_cf.mim`, `constrain.mim`, and `color.mim` run at a small `k` by default; `scripts/x86_ksweep.sh` runs the whole allowlist at `k = 2`, `3`, `4`, and `6` via `--param x86_k=<n>` against each test's expected exit codes, but no CI job runs it yet.
  At `k = 2`, nine tests stop with "needs more than 2 registers", the error without fallback described under *Spilling*.
- No lit test pins the parallel-copy cases directly (an `xchg` cycle, a memory-φ `push`/`pop`, callee-saved saves, the `trunc` division); only the manual sweep exercises them, at small `k`.
- The emit tests match registers by pattern now; they check the chosen instructions and address modes, not the assignment.
- Re-running the full pipeline on backend output is not meaningful (`core.legalize_idx` η-wraps `x86.trunc` to `Idx 4`); round-trip backend dumps with `--no-opt`.
- Loading `plugin x86` in a source file pulls `x86.emit` into the default pipeline; tests of isel or scheduling alone use a local `_compile` with just those phases.
- A user-file `axm` with a checked codomain dumped as `… → Idx _1` with `_1` unbound; likely a dumper round-trip bug.

Pitfalls already hit (fixed, but each is a rule for any new backend phase):

- A jump whose argument is a whole instruction (η-reduced `k i` for `k (i#0, i#1)`) has to be rewritten per projection (`Subst::rewrite_jump`), or its memory skips what was placed before the jump; this put the reload of a branch condition off the chain at `k = 2`.
- Rewriting a block in place must not let `World::app` inline a `tt`-filtered target or fold a branch whose targets became α-equal; freeze the filters to `ff` first.
- A callee that is a known block is a jump only if it is a basic block: the function itself is block 0 of its `Liveness`, so a self-recursive call looked like a jump to the spiller and escaped the caller-saved limit.
- An instruction off the chain is invisible to `Liveness`; `x86.color`'s checker now rejects any it reaches.

## Next Steps

1. **PBQP instruction selection**: rules are data and the solvability checks run over the rule set now (see *Solvability*); next is rules in bulk (`cc` for flags consumed by branches and `cmov`, scaled indices from `shl`/`mul`, three-operand `add` via `lea`, the `and` mask for `trunc` → `movzx`).
2. **Register allocation**: spilling, `x86.constrain`, `x86.color`, and `emit` on the colored registers (SSA destruction by parallel copies) are done; next are spill-slot coloring with memory-φ coalescing, loop-depth weights for copies, and pressure-aware list scheduling (see *Register Assignment*); each piece is checked against the allowlist, also at small `k`.

## Instruction Selection with PBQP

The base is Buchwald & Zwinkau, *Instruction Selection by Graph Transformation* (CASES 2010): selection is a cover of the program graph by matches of rewrite rules, found as a PBQP.
The one generalization is that the holes of a pattern are typed MimIR terms instead of always meaning "a value in a register".
This keeps their rule shape and solvability argument and adds the factoring of a grammar (Ebner et al., LCTES 2008).

### Rules

- A rule is `pattern ↦ replacement` plus a cost.
  Both sides are Mim terms over the same holes: the pattern over `core`/`mem`, the replacement over `x86`.
- A hole is a variable whose type is an x86 operand type: `Idx s` (a register), `Addr i`, or `Flags`.
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
- *size*: the operand size is one the instruction supports; this is the `refly.check` in the instruction's codomain, which isel evaluates for a literal size.
- *memory order*: nothing may lie between the memory operations a match covers; the matcher rejects such a match before the PBQP is built.
- *availability*: if the replacement sits at an inner node's place in the memory order (`x86.rm` sits at its load), the other holes must be available there.

### Solvability

- *Atomic completeness*: every operation has a rule with no inner nodes, an operand form, and holes only in `Reg`/`Base`/`Addr`; it is the fallback cover, and *total conversions* ensures its type is usable as a value.
- *Total conversions*: every type a rule may have converts to a register (`Addr` via `x86.lea`, a literal via `x86.mov`, flags via `setcc`), and every hole type is reachable from some rule type.
- *Composability* (the paper's Def. 12) is implemented for the inner nodes only: every inner slot of a rule needs an atomic rule for its `Op` (for `Op::Alu`, for every operation with a memory form), since the inner node must be coverable on its own.
  The general successor cut of a multi-node pattern is not checked yet; a cut at a typed hole is a rule boundary already.
- `check_rules()` runs these over the whole rule set on every `analyze()` in a checked build (`MIM_ENABLE_CHECKS`, on by default in Debug) and reports the offending rule by name.
- The solver deletes forbidden options and propagates the deletion, which is the paper's infinity propagation.
- The paper's proof covers untyped holes only; typed holes are a factored presentation of such a rule set, but recursive operand rules make that expansion infinite.
  So selection keeps the fallback: the atomic rules everywhere are always a valid cover.

### Replacement

- Selection runs in `ISel::analyze()`; the rewrite then applies the chosen alternatives.
- A root alternative builds its replacement with each hole substituted by the rewritten operand, converted where the edge said so.
- A node with an inner alternative is not rewritten on its own; its match's root builds it.

### Staging

- Now both sides of the rule set are `constexpr` tables in `isel.cpp`: `rules_`, one entry per operation and operand form with its slots, side condition, cost, and builder, and `patterns_`, one entry per recognized axiom with its `Tag`.
  `decompose` names no axiom: it switches over `Tag` to extract the operands, so the only code left is the operand extraction, the register sizes a `Tag` implies, the `Cond`s, and the builders of replacements that are more than one instruction (`nat.sub`, `select`, `slot`, `assume`, and the `negate`/`res`/`sign` flags of an `Insn`).
  Writing replacements as Mim terms is the next step; it also brings `Ty::Flags`, so that `select` fuses with a `cmp` by PBQP choice.
  The PBQP construction, the rewrite, and the solvability checks are generic over the tables.
- Later, the rule table is generated from Mim rules (*Pattern Database*); nothing else changes.

### Open points

- A multi-result pattern that should win only because it is shared (`sub` and `cmp` of the same operands) is underpriced by the one-rule-per-result scheme; to be revisited with flags.
- Flags: `cmp` + `cc` as a rule of type `Flags` consumed by branches and `cmovcc`; keeping the flags alive until then is the scheduler's job.
- The cost unit: µops or latency from uops.info for a chosen µarch, with encoding size as tie-breaker.

## Register Allocation

SSA-based allocation (Hack/Braun) with PBQP coloring, each step a phase on the World:

- Pressure-aware list scheduling.
- Spill first until MaxLive ≤ k (Braun & Hack, *Register Spilling and Live-Range Splitting for SSA-Form Programs*, CC 2009): done as `x86.spilling`, see *Status*.
- PBQP coloring with `rn_priority` set to PEO positions, so RN never fails on chordal SSA graphs (Buchwald, Zwinkau, Bersch, *SSA-based Register Allocation with PBQP*, CC 2011); `pbqp::solve` already supports this.
- Fixed-register instructions (`div` with rax/rdx, shifts by `cl`) are isolated by live-range splits right before and after them, so the graph stays chordal; calls, returns, and the entry are parallel copies instead.
- Two-address forms: if operand 0 is still live after the instruction, split it first, then add a tied edge between result and operand.
- φ copies become affinity edges (a move's cost off the diagonal) in the same PBQP instead of a separate coalescer.
- SSA destruction: block params stay as the φs; every jump is a parallel copy from its argument registers to the parameter registers, which `emit` sequentializes (`xchg` for cycles).

Copies, spills, and reloads at different program points stay distinct Defs because each takes its chain token (see *The schedule is the `mem.M 0` chain*).

### Register Assignment

The result of coloring lives in the graph: every use of a value is wrapped in

```
axm in: [r: I4] → {s: Nat} → Idx s → Idx s;
```

- `x86.in r v` is `v`, which lives in register `r` in hardware encoding order (rax = 0, rcx = 1, rdx = 2, rbx = 3, rsp = 4, rbp = 5, rsi = 6, rdi = 7, r8 = 8, …, r15 = 15).
  The lets `x86.reg.rax = 0I4` etc. form the module `x86.reg`, whose subtags are in that order; C++ takes `Num_Regs` and a register's encoding from them (`Annex::num<reg>()`, `encoding(reg::rax)`), so the registers are written down once.
- `I4` makes "a general-purpose register" a typing fact; the width comes from `s`, so al/ax/eax/rax need no names, and since one value occupies a whole physical register, subregisters cannot alias.
- `in` is no instruction: it is not scheduled and sits on no chain, like `x86.addr`.
- Not wrapped: literals (immediates), `mem.M`, `Flags` (one flags register), `Spill s` (a stack slot each, in `emit`), `Addr` itself (its base and index are wrapped inside it), and the `Bool` of a `cc` that only a branch consumes.
- `Liveness::values` already looks through non-instruction Defs, so `in r v` counts as a use of `v` without changes.

Use-wrapping is def-annotation:

- A value has one register for its whole live range; live ranges split only at jumps.
- Each value is a distinct Def (its chain token), so all its uses share one hash-consed `in r v`; wrapping the uses is the substitution `v ↦ in r v` over the function body, in place per block and without type changes.
- The output register of an instruction is recorded only there, as is the register of a block parameter; "where does `v` live" is a per-function map collected from the `in` nodes, and the graph stays the source of truth.
- A result nobody uses has no record of its register, so dead pure instructions are dropped before coloring; a dead result of a fixed-register instruction (`div`'s remainder) is a clobber known from the instruction itself.

Parallel copies are jumps:

- A jump `k (m, a, b)` to a block `k (m, a', b')` is the parallel copy `reg(a) → reg(a')`, `reg(b) → reg(b')`, as a call already does when it passes its arguments and receives its result in the return continuation's parameter.
- So there is no copy instruction: every live-range split is a fresh continuation whose parameters are the copies; live-through values stay free in it (it is dominated by the splitting block) and keep their registers.
- SSA destruction needs no graph rewrite; `emit` sequentializes each jump's moves.
- Branch targets share one argument tuple; `emit` performs each target's copies on its own edge, the then target's in a trampoline.
- A split continuation has one predecessor and follows it directly, so `emit` falls through to it.

Example (SysV: `a` in rdi, `b` in rsi, the result in rax):

```
con f (m: mem.M 0, a b: I64, ret: Cn [mem.M 0, I64]) =
    let (m, x) = x86.sched x86.add m (x86.in 7I4 a, x86.in 6I4 b);
    let (m, y) = x86.sched x86.imul m (x86.in 7I4 x, 3I64);
    ret (m, x86.in 7I4 y)
```

`add` writes `x` into rdi, which only `x86.in 7I4 x` states; the jump to `ret` is the copy rdi → rax.
A `div` is isolated as `pre (m, a)` into `con pre (m: mem.M 0, a: I64)`, which divides `x86.in 0I4 a`.

Invariants after coloring, checked by `x86.color` over a fresh `Liveness`:

1. Every use of a register-class value is `in r v`, and all `in · v` agree on `r`.
2. Values live at the same point have distinct registers.
3. Two-address: `reg(result) = reg(operand 0)`.
4. Fixed registers: operands and results of `div`/`idiv`, shifts by `cl`; clobbered registers are not those of operands.
5. Every result whose register its instruction does not imply has a use.
6. No value live across a call is in a caller-saved register.
7. No instruction is off the chain.

Call arguments and results, entry parameters, and returned values are no invariant: they are parallel copies that `emit` performs.
`x86.color` checks all of these after every function (there is no separate `x86.check_ra` phase).

Phases:

- `x86.constrain` splits live ranges right before fixed-register operands and two-address operands that stay live, and right after fixed results that stay live, and emits `in` for the precolored values only.
  A fixed *definition* does need a split after it: precolored for its whole live range, a quotient live across another `div` or a call would need two registers at once.
  Entry parameters and a return continuation's parameter are treated the same way without a graph change: they only prefer their ABI register, and `emit` copies them out of it.
  Call arguments and returned values need no split either: the jump is the parallel copy into the callee's parameter registers.
  An operand that is split off and stays live occupies two registers at the jump, so `x86.spilling` has to count it in `extra`; shifts by `cl` need it, too.
- `x86.color` builds one PBQP per function over the general-purpose class (16 options, rsp and rbp ∞): fixed operands and results (`constraint()`) are precolored; interference edges are ∞ on the diagonal, tie edges ∞ off it, jump-argument–parameter affinities cost a move off it; clobbers go into the cost vectors (`rdx` on `div`'s operands, caller-saved registers across a call); `rn_priority` is the position of the definition in an order that visits dominators first (see *Status*).
- `x86.emit` reads the registers from the `in` map, sequentializes each jump's moves, saves the callee-saved registers it uses, and falls through where its layout allows (see *Status*).

Each stage dumps and round-trips, so lit tests check `x86.in` directly.

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
  The allowlist is a set of `%if x86_64-linux %{ … %}` RUN lines in those tests (feature set in `lit/lit.cfg.py`): every test that `-p opt … -p x86` compiles and runs correctly, 50 in `lit/` besides `lit/x86/`.
  The others need calls to `malloc`/`free`, aggregate memory operations, unions, or `math`.
- `mim-pbqp-test` checks `solve` against `solve_exhaustive` on small instances; there is no `mim-x86-test` yet, and the register-allocation invariants are checked by `x86.color` on every compiled function instead.
- The optimizer-dump oracle for any change to the pre-isel pipeline.
