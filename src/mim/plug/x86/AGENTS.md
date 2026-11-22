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
  A literal is no PBQP node: it is hash-consed across the whole world and free to rematerialize, so each using option prices it locally (immediate 0, `mov` 2) and scheduling rematerializes a closed instruction right before each use.
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

### Floating point lives in XMM registers

- A `math.F pe` value is a whole XMM register, like an `Idx s` is a general-purpose one; the format `pe` is a family parameter of every float instruction and rule, since neither an operand nor a result names it.
- The register classes never mix: `Ty::FReg`/`Want::FReg` are separate non-terminals, `rules::Chains` only converts within a class (`rules::same_class`), and `x86.fmov` is the only conversion of a float literal into a register.
- `x86.fsel` is a compare and a branch around a `movaps`: the then-value lives in the result register, so its operand is tied like a two-address instruction's.
- Every XMM register is caller-saved, so a float value alive across a call is spilled; `x86.u2f`/`x86.f2u` exist only for an `i32` integer, since not every `u64` is an exact `f64`.

### Condition codes

- `x86.cc` and `x86.cmovcc` are families in hardware encoding order (`x86.cc.o` = 0, …, `x86.cc.g` = 15), so a code is a subtag rather than an operand: C++ compares `Axm::base()` and reads `Axm::sub()`.
- `x86.cc.l: Flags → Bool` is `setcc` as a value and `jcc` as a branch condition.
- `core.icmp` maps onto `x86.cc.<c> (x86.cmp (a, b))` for `e`, `ne`, `sl`, `sle`, `sg`, `sge`, `ul`, `ule`, `ug`, `uge`, and `x86.legalize` turns `core.ncmp` into the unsigned `core.icmp`: the rule family `icmp` of `x86_isel` takes the predicate and its condition code; the other predicates have no instance and are rejected for now.

## Status

Done (on `feature/x86`):

- `x86` plugin (`src/mim/plug/x86/`), with `mem.add_mem`, `core.legalize_dyn_idx`, and `mem.legalize_ptr` (each if `x86` is loaded), `x86.legalize`, `x86.isel`, `x86.schedule`, `x86.spilling`, `x86.constrain`, `x86.color`, and `x86.emit` at the end of the default pipeline in `opt.mim`; by then no axiom pins a continuation's type, so every continuation has its `mem.M`.
- `x86.mim`: `Flags`, `Addr` (`Base`, `Index`), `Spill`, `addr`, `scale_1`/`_2`/`_4`/`_8`, `index`, `lea`, `mov`, `rm`, `rm1`, `mr`, `load`, `store`, `add`, `sub`, `and`, `or`, `xor`, `imul`, `shl`, `shr`, `sar`, `movzx`, `movsx`, `trunc`, `cmp`, `test`, the condition-code families `cc` and `cmovcc`, `spill`, `reload`, `in` with the module `reg` of the register lets `x86.reg.rax` … `x86.reg.r15`, plus the size predicates.
  For floating point it adds `fin` with the module `freg` of `x86.freg.xmm0` … `xmm15`, `fmov` (a literal from the constant pool), `fload`/`fstore`, `FSpill` with `fspill`/`freload`, `fadd`/`fsub`/`fmul`/`fdiv`, `fsqrt`/`fabs`, the family `fcmp` in the order of `math.cmp`, `fsel`, and `i2f`/`u2f`/`f2i`/`f2u`/`f2f`.
- `core.legalize_dyn_idx` turns each `Idx n` with a dynamic `n` into `I64`, ignoring the modulus as `ll` does; it must run after the last specialization, which is why it sits here and not in `core.legalize_idx`.
- `mem.legalize_ptr` (`src/mim/plug/mem/phase/legalize_ptr.cpp`) runs after `ll.emit`, right before `x86.legalize`: every `mem.Ptr (T, a)` becomes the byte pointer `mem.Ptr (I8, a)`, an operation that needs a typed pointer gets a `core.bitcast` of it (`coerce_leaf`), and `mem.lea` becomes `mem.ptradd` with x86's layout: a literal byte offset, or `core.wrap.mul (conv.u i64 i, size)` for a dynamic index; `mem.assume` vanishes and `mem.mslot` gets its size in bytes.
  An index `conv.u n y` that `core.legalize_idx` or `core.legalize_dyn_idx` kept for the type of `lea` becomes `core.rem n y` for a literal odd `n` (an `and` for a power of two) and `y` for a dynamic one.
  So no rule mentions a pointee type, isel finds address modes in plain arithmetic, and every conversion it sees is between register sizes.
- `x86.legalize` (`src/mim/plug/x86/phase/legalize.cpp`) runs right before `x86.isel` and turns every `Nat` value and every pointer into an `I64`, so isel only sees register types.
  `Nat` literals stay `Nat` so they still work as type-level arguments, and become `I64` only as values.
  An operation that needs a typed pointer (`mem.load`, `mem.store`) gets a `core.bitcast` of the `I64` (`coerce_leaf`), which a generated matcher looks through (`x86::strip_ptr_cast`).
  `mem.ptradd` becomes `core.wrap.add`; `core.nat.add`/`mul` become `core.wrap`, `core.nat.sub` a select of `core.wrap.sub` and `0` on `core.icmp.ul`, `core.nat.div`/`rem` `core.div.udiv`/`urem` on a `⊥` memory, `core.ncmp` the unsigned `core.icmp`, `core.idx` and `core.idx2nat` a `core.conv.u`, and a `core.bitcast` between types that became equal its operand.
  `mem.mslot` of a literal size becomes `x86.slot`; a dynamic size is an error.
- `x86.isel` (`src/mim/plug/x86/phase/isel.cpp`) covers `core.wrap`, `core.bit2` (and/or/xor), `core.shr`, `core.icmp`, `core.conv`, `mem.load`/`mem.store` of integers, `core.rem`, `core.div`, integer `core.bitcast`, and a select `(x, y)#c` (as `cmovcc`).
  Of `math` it covers `arith.add`/`sub`/`mul`/`div` in `f32` and `f64`, `abs`, `rt.sq`, `cmp` for the fourteen ordered and unordered predicates (not `f` and `t`, which normalize to constant booleans), a float select, `mem.load`/`mem.store` of a float, and `conv.s2f`/`f2s`/`f2f` plus `conv.u2f`/`f2u` with an `i32` integer; a float literal becomes `fmov`.
  `ll.drop_hints` removes the loop hints `ll.vec`/`ll.novec` before (`opt.mim`).
  Any other `core`/`mem`/`math` operation left in the selected world is an error naming the axiom.
- `x86.schedule` (`src/mim/plug/x86/phase/schedule.cpp`) is a `NestPhase` that rewrites each function in place (it changes no types): it places every instruction of a function once with `Scheduler::smart` (as late as possible, but out of loops) and threads each continuation's instructions onto its `mem.M 0` chain in topological order.
  Memory instructions are rethreaded, existing `sched`s re-placed (so the phase is idempotent), all others wrapped in `x86.sched`; every other use of a memory token gets the end of the chain.
  A closed instruction has no place of its own (`Scheduler::is_remat`): it gets a fresh copy right before each use.
  So do `cmp`/`test` and `cc`, since flags are no register value: each consumer recomputes them right before itself, so at most one flags value is live and nothing clobbers it before its consumer.
  Instructions that `Scheduler::smart` hoists into a block are appended after that block's own ones, i.e., after the condition of its branch; so `chain()` appends a fresh `cmp`/`cc` for the branch at the very end.
  A continuation without a `mem.M`, a direct-style function with one, and a continuation whose body is no jump are errors.
- `Liveness` (`src/mim/plug/x86/liveness.cpp`) is a side table over one scheduled function: blocks with their chains, predecessors and successors, live-in and live-out sets (path exploration per use), and MaxLive per register class (general-purpose, floating-point, flags; spill slots are class `Stack` and unbounded).
  A value's class comes from its type: `math.F pe` is `Fpr`, `x86.Spill`/`x86.FSpill` are `Stack`, `x86.Flags` is `Flags`, everything else `Gpr`.
  A value is a non-memory result of a chain instruction or a non-memory parameter; a parameter is defined at block entry and a jump uses its arguments, so a parameter is never live-in.
  A branch on an `x86.cc` uses its flags; the `Bool` is a value only if something else uses it.
  `x86.live` prints it (`-X x86:live=<file>`) and is not part of the default pipeline.
- `x86.spilling` (`src/mim/plug/x86/phase/spilling.cpp`) brings MaxLive of the general-purpose class down to `k` (14 by default, `-X x86:k=<n>`) and then the floating-point class down to `k` (16 by default) in a second, independent pass over the function, per function and in place (Braun & Hack, CC 2009):
  - Global next-use distances by a fixpoint over the CFG, plus `2^20` across an edge into a smaller loop depth; loops are natural loops over the dominator tree (Cooper, Harvey, Kennedy) of the block graph, since `Nest` misses closed blocks.
  - `W_entry`: at a block whose predecessors are all processed, the candidates in a register at the end of all predecessors first, then those of some, by next use; otherwise (loop header) the candidates used in the loop first, then live-through ones only as far as the loop's MaxLive leaves room.
  - Per instruction: reload missing operands, `limit(W, k − extra)` (`div`/`idiv` need `rdx`), `limit(W, k − #defs)` after it; a call keeps at most `k − 9` general-purpose values across it (five callee-saved registers at `k = 14`) and no floating-point one, since every XMM register is caller-saved.
  - A parameter of the entry, a return continuation, or a branch target, and a dead parameter, is always in a register; any other parameter outside `W_entry` becomes a memory φ of type `x86.Spill s` or `x86.FSpill pe`.
  - A value is spilled right after its definition (or at its block's entry), and only if it is reloaded or flows into a memory φ; a literal flowing into a memory φ is spilled at the end of the predecessor.
  - A closed instruction is recomputed instead of reloaded.
  - Coupling reloads go to the end of the predecessor.
    EtaConv does not quite rule out critical edges: a hash-consed branch tuple can share one wrapper among several branches, so `x86.spilling` first gives each branch to such a target a fresh wrapper.
  - Reloads give a value several definitions; SSA reconstruction (Braun et al., CC 2013, every block sealed, union-find for trivial φ removal) runs symbolically first, so each block whose signature changes is rebuilt as a new Lam exactly once with its memory φs and new φ parameters appended.
  - `spill`/`reload` and `fspill`/`freload` are pure instructions on the chain via `sched`; `x86.emit` gives every `Spill`/`FSpill` value a stack slot of its own and turns them into stores and loads.
- `x86.constrain` (`src/mim/plug/x86/phase/constrain.cpp`) runs after `x86.spilling`: right before an instruction with a fixed operand (`div`'s dividend in `rax`, a shift count in `rcx`) or a two-address operand that stays live, the block jumps to a fresh continuation that takes these operands; the fixed copies are wrapped in `x86.in`.
  Likewise, right after an instruction with fixed results (`div`'s quotient in `rax` and remainder in `rdx`), the live ones go to a fresh continuation as `x86.in rax q`, `x86.in rdx r`, so a precolored live range ends at once; this costs no extra register, as the arguments die where the parameters are defined.
  The table of fixed operands, tied operands, and fixed results is `constraint()` (`src/mim/plug/x86/constraint.cpp`), shared with the spiller and the colorer.
  `x86.spilling` counts these copies as `extra` registers; if an instruction would then need more than `k`, the operands to copy leave their registers at it instead and are reloaded at their next use.
- `x86.color` (`src/mim/plug/x86/phase/color.cpp`) runs after `x86.constrain`, per function and in place:
  - It first drops chain instructions none of whose results is used (to a fixpoint), so a result without a use is one whose register its instruction implies (`div`'s remainder); and it sets every block's filter to `ff`, since rebuilding a jump must not inline its target (pruning can make both targets of a branch α-equal, and the World then folds the branch).
  - One PBQP per register class with 16 options: `rsp` and `rbp` are ∞ and callee-saved registers cost 1 in the general-purpose one, all XMM registers are caller-saved and reserved ones do not exist in the floating-point one.
    A dead parameter is no node.
    `-X x86:color-dot=<file>` writes each function's graph in Graphviz DOT format with the chosen options highlighted.
  - Interference edges (∞ on the diagonal) between a definition and every value live after it or defined along with it; tie edges (∞ off the diagonal) between a two-address result and its tied operand; fixed operands and results are precolored (general-purpose only); clobbers are ∞ on the operands (`rdx` at `div`); values live across a call are ∞ on the nine caller-saved registers (`x86.in`) or on all 16 (`x86.fin`).
  - Copies are preferences: a jump argument and its parameter get an edge that costs 4 off the diagonal; entry parameters prefer their SysV argument registers (`rdi`…`r9`, `xmm0`…`xmm7`), call arguments theirs, a return continuation's parameter and a returned value prefer `rax`/`xmm0`.
  - `rn_priority` is the node id, and nodes are created in block discovery order (which visits a dominator first) and within a block in definition order, so RN eliminates along a PEO.
  - The rewrite wraps every use in `x86.in r v`, or an XMM value in `x86.fin r v`; a whole tuple of values (a block's variable, or an instruction passed whole after η-reduction) is rebuilt from its wrapped projections.
  - A checker then recomputes `Liveness` on the result, once per class, and verifies the invariants below; any violation is an error naming the values.
- `x86.load`/`x86.store` work on `mem.M 0`; isel collapses `mem.M a` to `mem.M 0`.
- `x86.emit` (`src/mim/plug/x86/emit.cpp`) writes GAS text in AT&T syntax to `<world>.s` (`-X x86:o=<file>`) on the registers of `x86.color`:
  - It walks each function's blocks with their chains from `Liveness` and reads "where does `v` live" off the `x86.in`/`x86.fin` nodes (plus the implied registers of fixed results); an operand that is no `x86.in`/`x86.fin` and no literal is an error.
  - Instructions use the registers directly; a two-address result is in the register of its tied operand, which `assert`s check.
  - A jump, a call (arguments into the SysV registers, then the result from `rax`/`xmm0` into the return continuation's parameter), a return (into `rax`/`xmm0`), and the entry (from the SysV registers into the parameters) are parallel copies: spill slots first via `push`/`pop` (memory φs, cycles included), then register moves in dependency order with `xchg` for cycles (three `xorps` for XMM ones), then immediates; a dead parameter receives nothing.
  - Flags are emitted where `x86.schedule` placed the `cmp`/`test`; `emit` tracks which result the flags hold and rejects a consumer whose flags something clobbered in between.
  - The frame is `rbp`-based: the callee-saved general-purpose registers that hold values are pushed after `rbp`, and spill slots and `x86.slot` areas follow, aligned so that `rsp` is a multiple of 16 at calls.
  - A float literal is a 16-byte-aligned constant in one `.rodata` pool per output file (`f64` uses both halves, `f32` the low one), loaded RIP-relative by `fmov`; `fabs` takes its mask from the same pool and `u2f`/`f2u` their 2^32.
  - A float comparison is `ucomiss`/`ucomisd` plus `setcc`; an unordered one sets ZF, PF, and CF, so `e`, `l`, `le`, `ug`, `uge`, and `une` need a `jnp` over a fixup, and `f`/`t` are a constant.
    Isel only produces `e` and `une` of these: `x86_isel` turns `l`/`le`/`ug`/`uge` into the converse `g`/`ge`/`ul`/`ule` of the swapped operands.
  - `fsel` tests the condition and branches around a `movaps`.
  - `i2f`/`f2i` are `cvtsi2ss`/`cvttss2si` at the operand's size; `u2f` from `i32` converts signed to `f64` and adds 2^32 after a `testl`/`jns`, then rounds once to `f32` if need be, `f2u` to `i32` converts to `i64` and truncates; `f2f` is `cvtss2sd`/`cvtsd2ss`.
  - Blocks are laid out by chasing each block's last jump (a jump's target, a call's return continuation, a branch's else target or its trampoline's target) and fall through to the next block where possible.
  - A non-power-of-two `trunc` is a `div` with `rax`, `rcx`, and `rdx` saved around it by `push`/`pop`, as nothing reserves them.
  - A non-external callee is emitted as a local function `<name>_<gid>`; an external declaration is called by its symbol.
- `x86.cmovcc.l (f, x, y)` yields `y` if `l` holds, like `(x, y)#cond`.
- `x86.isel` follows *Instruction Selection with PBQP*: the rules are data, the `rules::Rewrite` rows of `x86_isel` (see *Staging*).
  An `Alt` points to its row directly, and a hole has the row's `rules::Want`, whose `Param`/`Lit` never convert; the row's `rules::Ty` is the non-terminal of the replacement.
  One instruction is one rule per operand form (`alu`, `rm`); a family that takes the operation names a row after the root of its lhs (`alu(core.wrap.add)`).
  The marker `x86_isel.reg_imm x` (`reg_sh` for a shift count) makes a hole `RegImm` (`RegSh`): `append` resolves it to `Imm` (`Sh`) for a literal that fits and to `Reg` otherwise, so the register and the immediate form are one rule.
  A swap that only yields another match with an immediate (`Rewrite::imm_swaps`) is dropped when the match has none.
  A rule that maps a cover's memory (an `rm` rule) requires the availability of the root's holes at the load whose memory the replacement takes over (`holds`).
  `root_of` says which rules a Def is a root for: those of its axiom if some rule has it as root, those of `0` if it is an Extract at a dynamic index; every other Def is no node.
  The generator puts the rules of a root next to each other and indexes them in `rules::roots`; a matcher does not check the root's axiom again.
  Every rule is a rule of `x86_isel`.
  An Extract at a dynamic index has root `0`, which is how the rules for a select `(x, y)#c` apply.
  A cast between pointer types is a no-op to isel: a generated matcher binds a pointer hole to what is under it (`x86::strip_ptr_cast`), and the rewrite drops it.
  Which operand sizes an x86 instruction takes is stated by the `refly.check` in its codomain; the instance lists of `x86_isel` only name sizes it accepts.
  `match` tries every rule with the Def's root; an operand of a node is a projection of its argument; an inner node must be `sole` (its value and its parent's operand tuple have no other user).
  A match is an `Alt` with typed `Demand`s on the operands and the inner nodes it covers (`Cover`).
  `ISel::analyze()` builds the PBQP generically from that: every operation with a match is a node, inner alternatives are the classes of covers with equal demands (resolved bottom-up), edges price `conv` between rule type and hole type, and all-zero edges are left out.
  Conversions are rules of `x86_isel` whose lhs is all of their meta variables (`rules::conversions`: `base` makes a register an `x86.Base`, `lea` an address a register, `scale1`/`unscale` a register a `Scaled` and back, and under the guard `x86_isel.lit x` a literal `abs` an absolute address and `mov` a register); `rules::Chains` takes the cheapest chain between any two non-terminals at compile time (`rules::chains`), `conv` prices a hole by it, and `convert` replays it.
  A literal operand or a literal replacement is `Ty::Lit` and fills `Imm`/`Sh` as it is; `key` lets the value projection of any memory operation stand for it.
  The `x86.addr` normalizer folds a literal base into the displacement if the sum fits (else it moves it into a register), and the `x86.lea` normalizer reduces `addr (0, b)` to `b` and `addr (d, 0)` to `d`.
  The rewrite is generic too: a root alternative's rule builds the replacement, `hole` converts an operand's replacement from its rule's non-terminal to the hole's type, and an inner node defers to the root of its match.
  The fallback cover takes each node's first atomic alternative instead of trusting the table order, and `x86_isel.gen` verifies the rule set itself (see *Solvability*).
  `-X x86:isel-dot=<file>` writes the graph in Graphviz DOT format with the chosen options highlighted.
- `Want::Sh` is a shift count: any literal, since x86 masks it, where `Want::Imm` must fit a sign-extended `i32`.
- `x86.emit` prints a literal operand as `$c` (it must fit a sign-extended `i32`) and rejects one where the instruction needs a register.
- Address rules: a pointer is an `I64`, so the address forms are rules of `core.wrap.add` (`index_disp`, `base_index`, `disp`, `index`), declared after `arithmetic` to lose a tie against its `add`.
  The `mem.ptradd` normalizer merges nested offsets, and the `core.wrap.add` normalizer pulls a literal out of a nested sum, so a displacement sits outermost.
  A literal summand that fits `i32` is the `disp`, `p + (x + i)` puts `p + x` into the base with an `x86.lea`, and any other summand is the index.
  Since a base and an offset commute, which of two equally cheap covers isel picks depends on the solver, e.g. `lea (r + c·4) + a` instead of `lea (a + r) + c·4` for a row-major access (`isel_addr.mim`).
  An index is the non-terminal `Scaled` (`[I64, I2]`, the hole marker `x86_isel.scaled i`): the `scale` family makes a `core.wrap.mul` by 2, 4, or 8 or a `core.wrap.shl` by 1, 2, or 3 one, any other register takes scale 1, so a product shared by several addresses is one node that each folds.
  `base_index` keeps `x` a register, so it and the scaled `i` differ and both orders of the `add` stay matches; with both `Scaled`, the generator drops the swap and `p + i * 4` can land in the `lea` of a loop.
  Of two matches that cost the same, the solver keeps the first, so the rules come larger first; within a root, the table follows the declaration order of `x86_isel`'s annexes, where a closed `rule` is an annex of its own and a family's instances sit in their tuple.
- `<op>_rm` folds a sole load into `add`/`sub`/`and`/`or`/`xor`/`imul` (and `float.rm`/`float.cmp_rm` into the float arithmetic and comparisons) as `x86.rm f (m, a, addr)` (`op r, addr`): the user moves to the load's place in the memory order, so its other operand must be in scope of the load's block and must not depend on the load.
  `mr` folds a store right behind the load (the load's memory has no other user) of an operation on the loaded value to the same address into `x86.mr f (m, addr, b)` (`op addr, r/imm`; not `imul`).
  `cmp`/`test` do not fold since `x86.schedule` rematerializes them per consumer, and `div` does not since `cltd` clobbers the index register.
- `x86.div`/`idiv` yield quotient and remainder as a pair, so a quotient and a remainder of the same operands share one instruction; `x86.slot n` reserves `n` frame bytes on the `mem.M` chain; `trunc` to a non-power-of-two size is an unsigned remainder.
- The rules `mul8`, `div8`, `rem8`, and `select8` widen 8-bit operations to `i32` (`movzx`/`movsx`, the instruction, `trunc`), since `imul`, `div`, `idiv`, and `cmovcc` have no 8-bit form.
- A select is `cmovcc.ne (test (c, c), x, y)`; `x86.cmovcc`'s normalizer turns it into `cmovcc.l (f, x, y)` if `c` is `cc.l f`, so a `cmp` feeding a `setcc` and a `cmov` stays one.
- `core.nat.sub` saturates as `cmovcc.b (cmp (a, b), sub (a, b), mov 0)` via the select rule, and `core.rem` puts its divisor into a register with one closed rule per size, since `x86.mov`'s result type only folds for a literal size.
- Tests: `lit/x86/types.mim`, `lit/x86/isel.mim` (with `-o` round-trip), `lit/x86/isel_imul8.mim`, `lit/x86/isel_nlogic.mim`, `lit/x86/isel_mr_shared.mim`, `lit/x86/isel_index_shared.mim` (a shared scaled index), `lit/x86/isel_index_shl.mim`, `lit/x86/isel_disp_shared.mim` (look-through), `lit/x86/isel_abs.mim` (literal addresses), `lit/x86/isel_addr.mim` (merged offsets), `lit/x86/isel_rules.mim` (the generated C++), `lit/x86/isel_select.mim`, `lit/x86/isel_lea.mim` (an `add` that would need a copy), `lit/x86/isel_slot.mim`, `lit/x86/isel_slot_dyn.mim` (a dynamic size is an error), `lit/mem/legalize_ptr_index.mim`, `lit/x86/conv_fold.mim`, `lit/x86/addr_fold.mim`, `lit/x86/emit_run.mim`, `lit/x86/emit_nat.mim` (the `Nat` lowering of `x86.legalize`), `lit/x86/emit_cf.mim` (loop with a parameter swap, recursion), `lit/x86/emit_addr.mim` (folded address modes), `lit/x86/emit_rmw.mim` (`rm`/`mr`), `lit/x86/emit_pbqp.mim` (competing folds), `lit/x86/sched.mim`, `lit/x86/sched_join.mim` (one placement in the dominator), `lit/x86/sched_loop.mim` (hoisting), `lit/x86/sched_remat.mim`, `lit/x86/sched_closed_tuple.mim`, `lit/x86/sched_no_mem.mim`, `lit/x86/sched_no_chain.mim`, `lit/x86/live_loop.mim`, `lit/x86/live_call.mim`, `lit/x86/size_imul8.mim`, `lit/x86/size_add5.mim`, `lit/x86/spill_loop.mim` (live-through values spilled outside the loop), `lit/x86/spill_phi.mim` (memory φ), `lit/x86/spill_ssa.mim` (new φ for a reloaded value), `lit/x86/spill_call.mim` (callee-saved limit), `lit/x86/spill_float.mim` (a float across a call), `lit/x86/constrain.mim` (also at `k = 3`), `lit/x86/color.mim` (fixed results, a value across a call; also at `k = 3`), `lit/x86/isel_float.mim` (the float rules, with `-o` round-trip), `lit/x86/emit_float.mim` (float arguments and the pool), `lit/x86/emit_float_cmp.mim` (NaN semantics of every predicate kind, two fixups), `lit/x86/emit_float_unary.mim` (abs, sqrt, div, select, `f2f`), `lit/x86/emit_float_u2f.mim` (unsigned conversion, single rounding to `f32`), `lit/x86/emit_float_spill.mim` (an `fspill`/`freload` pair in the assembly), `lit/x86/emit_float_rm.mim` (folded float loads into arithmetic, `ucomiss`, a conversion, and `sqrt`); `lit/x86/emit_cf.mim` also runs at `k = 3`.

Known gaps and bugs, as of 2026-11-22.

Open bugs and risks (wrong code or a crash is possible, or only a later check catches it):

- `x86.reg`, `x86.cc`, and `x86.cmovcc` have to stay in hardware encoding order, and nothing checks it: C++ reads registers and condition codes off the subtag (`encoding`, `emit`'s mnemonic table, `normalize_cmovcc`'s `cc` ↦ `cmovcc`), so a reordering silently emits wrong registers or conditions while a register's Mim value stays as written.
- A use of a fixed-result tuple as a whole (`div`'s `insn#1` instead of its projections) bypasses `x86.constrain`'s post-split: `Subst` maps the projections to the new continuation's parameters, but `insn#1` itself to the new instruction's, so that use would see `rax`/`rdx` past the split; untested, `x86.color`'s checker would report it.
- A two-address instruction whose tied operand is a literal (`sub (5, x)`) has no tie (`constraint()` drops it), so nothing keeps the result out of the other operand's register; `emit` rejects the literal ("needs an `x86.mov`"), and isel must never produce it.
- `emit` checks two-address results only with `assert`s, so a release build relies on `x86.color`'s checker.
- PBQP coloring has no fallback: an infeasible solution is an error naming the conflicting values.
  Chordality plus the PEO priority rules this out for unconstrained values, but precolored values and the ∞ entries for clobbers and calls void that guarantee in theory.
- `emit`'s flags tracking turns a clobbered flags value into a compile error; it can still happen where two `cmovcc`s share one hash-consed `cmp` copy (the memoization noted below under *Scheduling*) and a flag-clobbering instruction lands between them.
- `prune` in `x86.color` drops a dead `load`, which removes a possible fault; volatile accesses are not modeled.
- `prune` sets every block's filter to `ff` for good, so backend dumps show η-wrappers without their `tt` filter.
- The two spilling passes rewrite one function in sequence, and the second maps the other class' live-ins to the defs the first built; running one class again on its own output would leave those defs dangling.

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
- A call keeps no floating-point value in a register, since every XMM register is caller-saved: a float alive across a call costs a slot even at `k = 16`.
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
- Every function sets up `rbp`, even a leaf without slots, and every call sets `eax` to the number of XMM arguments, even for a non-variadic callee.
- `x86.emit` handles neither switches with more than two targets, indirect calls, more than six arguments, nor `movsx`/`trunc` to non-power-of-two `Idx` sizes; it does not prefix symbols for Mach-O.

Instruction selection:

- A looked-through inner node keeps the cost of its own selection in the PBQP even if no chosen alternative uses its value; the rewrite then never builds it.
- A swap that only exchanges two holes of the same want gives an alternative of the same PBQP cost, so the generator drops it; but the cost model does not see loop invariance, so which operand lands in a `lea` is the program's order (see `base_index`).
- The `load`/`store` rules take `mem.M 0` and a pointer only as `mem.Ptr0 T`, so a memory operation in another address space has no rule.
- `isel` still emits size-check failures for non-register sizes (e.g. `core.wrap.add` on `Idx 5`); these need masking or a clear rejection.
- An array index that `core.legalize_idx` narrowed is still reduced modulo its size in `mem.legalize_ptr` (an `and`, or a `div` for a size that is no power of two), although it is in range.
- `malloc`/`free`, aggregate loads and stores, `bitcast` of unions or floats, and `core.mulrem` are not selected; `core.nat.div`/`rem` do not guard against zero like `ll`, and `x86.legalize` gives their `core.div` a `⊥` memory, since a `Nat` operation has none.
- Floating point is `f32` and `f64` only: `math.F` of another format (`f16`, `bf16`, `nvtf32`, …), vector types, `math.arith.rem`, `math.extrema`, the transcendentals, and `is_finite` have no rule, and `fmov` rejects another format.
- `u2f`/`f2u` exist for an `i32` integer only; an `i64` one would need a 2^63 split, and `emit` rejects it.
- `x86.rm` takes any operand type and is tied only if its operation is; `float.rm` folds a load into `fadd`/`fsub`/`fmul`/`fdiv` and `float.cmp_rm`/`float.swap_rm` into `ucomiss`, as its source: the second operand of a direct predicate, or the first of `l`/`le`/`ug`/`uge` and the symmetric ones via the converse; `x < y` with `y` loaded stays a load, since `seta` needs `y` in the register.
- `x86.rm1 f (m, addr)` is a unary operation on a loaded value: `s2f`, `f2s`/`f2u`, `f2f`, and `sqrt` fold a sole load, `u2f` (which reads its operand twice) does not; so `x86.f2i`/`f2u`/`f2f` take their source format explicitly, since a builder drops implicit arguments and nothing would determine it.
- SSE has no `op addr, r` form, so there is no float `mr`; a constant still costs an `fmov` instead of a pool operand.
- A float select is `fsel`, which branches and materializes both sides; there is no `blend`-style form.
- The operand markers of `x86_isel` are a hack: opaque axioms that only the generator understands, so a rule applied by `▷` leaves them stuck and the rules are no executable specification.
  `addr` and `scaled` cast a meta variable from its lhs type (`mem.Ptr0 T`, `I64`) to an x86 operand type so the rhs type-checks, and double as its `Want`; since `x86.legalize` lowers `Nat` and pointers to `I64`, only these two remain, and they mark an address and a scaled index, not a change of representation.
  `reg_imm`/`reg_sh` only state an operand form; which operand of an instruction may be an immediate is a machine fact like `insn::tied`, so `insn.h` should state it and the generator derive `RegImm`/`RegSh` from the hole's position, which also drops the family parameter `r: mark`.

Frontend bugs that `x86_isel` hits (libmim, not fixed):

- Unification has no occurs check: `x86.mr x86.add (…)` in a rule solves `?s := gpr ?s`, and `Def::zonk` recurses until the stack overflows; the `mr` rules pass `x86.add @ s` explicitly instead.

Testing and tooling:

- Only `emit_cf.mim`, `constrain.mim`, and `color.mim` run at a small `k` by default; `scripts/x86_ksweep.sh` runs the whole allowlist at `k = 2`, `3`, `4`, and `6` via `--param x86_k=<n>` against each test's expected exit codes, but no CI job runs it yet.
  At `k = 2`, ten tests stop with "needs more than 2 registers", the error without fallback described under *Spilling*.
- No lit test pins the parallel-copy cases directly (an `xchg` cycle, a memory-φ `push`/`pop`, callee-saved saves, the `trunc` division); only the manual sweep exercises them, at small `k`.
- The emit tests match registers by pattern now; they check the chosen instructions and address modes, not the assignment.
- Re-running the full pipeline on backend output is not meaningful (`core.legalize_idx` η-wraps `x86.trunc` to `Idx 4`); round-trip backend dumps with `--no-opt`.
- Loading `plugin x86` in a source file pulls `x86.emit` into the default pipeline; tests of isel or scheduling alone use a local `_compile` with just those phases, starting with `mem.legalize_ptr` (and `core.legalize_dyn_idx` for dynamic `Idx` sizes) and `x86.legalize`.
- A user-file `axm` with a checked codomain dumped as `… → Idx _1` with `_1` unbound; likely a dumper round-trip bug.

Pitfalls already hit (fixed, but each is a rule for any new backend phase):

- A jump whose argument is a whole instruction (η-reduced `k i` for `k (i#0, i#1)`) has to be rewritten per projection (`Subst::rewrite_jump`), or its memory skips what was placed before the jump; this put the reload of a branch condition off the chain at `k = 2`.
- Rewriting a block in place must not let `World::app` inline a `tt`-filtered target or fold a branch whose targets became α-equal; freeze the filters to `ff` first.
- A callee that is a known block is a jump only if it is a basic block: the function itself is block 0 of its `Liveness`, so a self-recursive call looked like a jump to the spiller and escaped the caller-saved limit.
- An instruction off the chain is invisible to `Liveness`; `x86.color`'s checker now rejects any it reaches.
- A builder of `x86_isel.gen` drops an implicit argument for `World::implicit_app`, so `(x86.f2i @ pe) s` lost `pe` where no operand determines it; write such a parameter explicitly in the axiom.
- `Reg` equality includes the register class; a `Reg` built from a bare number is general-purpose, which emitted a `movq %rax, %rax` for a float return.
- A `Lit` meta's type is not checked when matching, so the format-generic `fmov` matched `math.F16` although its rule instances name only `f32`/`f64`; `emit` rejects another format.
- `use_reg` must accept `x86.fin` as well as `x86.in`, or every XMM operand fails as "not in a register".
- A pass that rebuilds blocks must map a live-in of the other register class to the def the earlier pass built for it, since its defining block was rebuilt and the old def no longer dominates the use.

## Next Steps

1. **PBQP instruction selection**: rules are data and the solvability checks run over the rule set now (see *Solvability*); next is rules in bulk (`cc` for flags consumed by branches and `cmov`, the `and` mask for `trunc` → `movzx`).
2. **Register allocation**: spilling, `x86.constrain`, `x86.color`, and `emit` on the colored registers (SSA destruction by parallel copies) are done; next are spill-slot coloring with memory-φ coalescing, loop-depth weights for copies, and pressure-aware list scheduling (see *Register Assignment*); each piece is checked against the allowlist, also at small `k`.
3. **Floating point**: `f32`/`f64` run through every phase; next are `i64`↔float (an unsigned convert needs the 2^63 split), pool constants as memory operands, callee-saved XMM registers, `f16`/`bf16` (with the VEX zeroing of the upper halves), and calling libm for the transcendentals.

## Instruction Selection with PBQP

The base is Buchwald & Zwinkau, *Instruction Selection by Graph Transformation* (CASES 2010): selection is a cover of the program graph by matches of rewrite rules, found as a PBQP.
The one generalization is that the holes of a pattern are typed MimIR terms instead of always meaning "a value in a register".
This keeps their rule shape and solvability argument and adds the factoring of a grammar (Ebner et al., LCTES 2008).

### Rules

- A rule is `pattern ↦ replacement` plus a cost.
  Both sides are Mim terms over the same holes: the pattern over `core`/`mem`, the replacement over `x86`.
- A hole is a variable whose type is an x86 operand type: `Idx s` (a register), `Addr i`, a scaled index `[I64, I2]`, or `Flags`.
- `Addr: Bool → *` states in its type whether the address has an index, with the aliases `Base = Addr ff` and `Index = Addr tt`: `x86.addr (disp, base): Base` and `x86.index a (index, scale): Index` for `a: Base`.
  So "at most one index" is a typing fact: the dynamic-index rule has a hole of type `Base`, the constant-offset rule is polymorphic in `i`, and a second index converts `Index` to `I64` (`x86.lea`) and on to `Base`.
- The `mem.M` token is order only: it is neither a hole nor a result of a pattern and no PBQP edge; the replacement rewires it.
- An instruction with several results is tuple-valued, with one single-root rule per result: `udiv (a, b) ↦ (x86.div (a, b))#0` and `urem (a, b) ↦ (x86.div (a, b))#1`.
  Hash-consing merges the replacements into one instruction; the paper's patterns have one root, and this needs neither proxy nodes nor rules with two roots.
  The solver prices such an instruction once per result, which is harmless as long as no cheaper alternative competes.
  The type of a rule is the type of its replacement.
- A literal in a pattern is a pattern node, not a hole, like `Const` in the paper's `Add+Const`; it carries a range condition.
  Literals are no PBQP nodes (see *Instruction types are plain integers*), so they may overlap freely, and a register hole that lands on one costs an `x86.mov`.
- Operand forms are rules of their own, written once: `core.wrap.mul (i, 4) ↦ (i, scale_4)` has type `[I64, I2]`, and `core.wrap.add (p, i: [I64, I2]) ↦ x86.index (x86.addr (0, p)) i` has type `Index`.
  Instruction rules only mention the hole: `mem.load (m, p: Addr i) ↦ x86.load (m, p)`.
  So the rule count is instructions plus operand forms; `mem.ptradd` chains of any length fold, since its normalizer merges them into one offset.
- A multi-node instruction is a graph pattern with typed inner nodes, as in the paper: `add (a, (mem.load (m, p: Addr i))#1) ↦ x86.rm x86.add (m, a, p)`.
- A conversion is a rule whose pattern is a single hole: `a: Addr i ↦ x86.lea a`, `c ↦ x86.mov c` for a literal `c`.

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
- *size*: the operand size is one the instruction supports; this is the `refly.check` in the instruction's codomain, and the instance lists of `x86_isel` only name such sizes.
- *memory order*: nothing may lie between the memory operations a match covers; the matcher rejects such a match before the PBQP is built.
- *availability*: if the replacement sits at an inner node's place in the memory order (`x86.rm` sits at its load), the other holes must be available there.

### Solvability

- *Atomic completeness*: every operation has a rule with no inner nodes, an operand form, and only holes that every non-terminal converts into (`Reg`, `Base`, `Addr`, `Scaled`); it is the fallback cover, and *total conversions* ensures its type is usable as a value.
- *Total conversions*: every type a rule may have converts to a register (`Addr` via the conversion rule `lea`, a literal via the conversion rule `mov`), and every hole type is reachable from some rule type.
- *Composability* (the paper's Def. 12) is not checked since the inner nodes moved to generated rules, whose inner axioms are only known per match; each root needs an atomic rule, since the inner node must be coverable on its own.
  The general successor cut of a multi-node pattern is not checked either; a cut at a typed hole is a rule boundary already.
- `x86_isel.gen` checks these over the whole rule set with the `rules::Chains` that isel uses and fails the build naming the offending rule.
- The solver deletes forbidden options and propagates the deletion, which is the paper's infinity propagation.
- The paper's proof covers untyped holes only; typed holes are a factored presentation of such a rule set, but recursive operand rules make that expansion infinite.
  So selection keeps the fallback: the atomic rules everywhere are always a valid cover.

### Replacement

- Selection runs in `ISel::analyze()`; the rewrite then applies the chosen alternatives.
- A root alternative builds its replacement with each hole substituted by the rewritten operand, converted where the edge said so.
- A node with an inner alternative is not rewritten on its own; its match's root builds it.

### Staging

- `isel.cpp` has no rules of its own; `ISel::build` is the only builder.
  The PBQP construction, the rewrite, and the solvability checks are generic over the tables.
- The rules move to Mim: the plugin `x86_isel` (`src/mim/plug/x86_isel/x86_isel.mim`) holds rule families, their instance tuples, and the phase `x86_isel.gen`, which writes every closed rule reachable from its annexes to `build/include/mim/plug/x86/isel_rules.h` (types in `rule_table.h`): one C++ matcher per distinct lhs, one builder per distinct rhs, and a `Rewrite` row per rule with its cost, root axiom, non-terminal, and the `Want` of each meta.
  The non-terminal of a rule and the want of a meta come from one classification of their types: `x86.Addr ff`/`tt` is `Base`/`Index` (`Addr` for a meta of a dynamic `i`), `[I64, I2]` is `Scaled`, anything else a register; a memory operation's rhs counts by its value, and an rhs of a dynamic address form is an error.
  A meta of type `Idx s` takes its want from a marker (`x86_isel.scaled`), else it is a register.
  An atomic rule has only holes that every non-terminal converts into, which `conv` decides.
  A matcher checks the lhs structurally, binds the metas (a hole also matches its type, which checks the size), and matches a tuple pattern by projections.
  A saturated axiom inside an lhs is an inner node (a cover), unless it is a type such as the implicit `core.Ptr (I8, 0)` of a `mem.load`; the matcher records each reference to one (`Ref::Value` as an operand, `Ref::Mem` by its memory), and the generator numbers the covers in preorder of their Value refs.
  Each node of an axiom listed in `x86_isel.commutes` gets a swap bit, also if its operand pair is the rule's meta variables as a whole (`(a, b)` η-collapses into the Var), and a rule's `swaps` keeps the bits whose two operand patterns differ under its wants.
  An lhs `(m, e)` replaces `e` and, by the memory of the rhs, the memory `m` of an inner node of `e` (`maps`); this is how `x86.rm` takes over a load.
  `isel.cpp` uses each row as it is: every subset of `swaps` yields a match whose Value refs must be `sole`, whose Mem refs must have no other user, and whose operand metas become `Demand`s of their owner; `ISel::build` converts each operand meta with `hole`, calls the builder, and maps the cover whose memory it replaces.
  Builders drop implicit arguments for `World::implicit_app`, so the holes determine them, e.g. the `i` of the `x86.Addr i` that the marker `x86_isel.addr p` stands for.
  A pure inner node whose value something else uses is looked through (`Cover::through`): the match takes its operands but leaves the node to its own selection, so a shared inner node of an address still folds into each; a memory operation never is.
  Rules apply by their exact root axiom, so a member of an annex without a rule is no node.
  The float rules sit in `pub mod float` with local type aliases for their family parameters, each instance tuple right after its rules; a rule's name is its own sym, so rows read `rm(math.arith.add)`, not `float.rm(…)`.
  The ALU forms (reg, imm, widened `mul8`, `Bool` logic, `nand`/`nor`/`nxor`), the comparisons, the selects, the conversions, `core.nat`, `core.div`, `core.rem`, loads and stores, the memory forms (`rm`, `mr`), the address forms, and the float arithmetic, comparisons, select, conversions, and memory forms are generated now, with selection byte-identical up to gids; `x86.movzx`/`movsx`/`trunc` fold a literal, so a widened immediate stays one.
  The generator derives what a rule wants of each meta variable: a meta the rhs does not use, another meta's type mentions, or of type `mem.M` or a continuation is a `Param`, otherwise its type gives `Reg` (`Idx s`) or `FReg` (`math.F pe`), its marker `Addr` (`mem.Ptr` under `x86_isel.addr`; a `Nat` is a `Param`, like the divisor of `core.rem`) or `Base`/`Index`/`Addr` (`x86.Addr`), and the guard atoms `x86_isel.imm`/`sh` give `Imm`/`Sh` and `x86_isel.lit` makes a `Param` a `Lit`, which must match a literal (and a conversion one from a literal); any other guard is an error, and a guard of `ff` drops the instance.
  A rule costs the summed `x86::insn::cost` of its rhs (`include/mim/plug/x86/insn.h`): each instruction's latency on recent cores (uops.info) plus one, so of two equally fast covers the shorter wins; an address costs nothing, `rm`/`mr` a load plus their operation, and a `trunc` to a register size nothing.
  `Rewrite::tied` marks the meta variables a two-address instruction of the rhs overwrites (`insn::tied`, which `constraint()` shares); an alternative adds `insn::Copy` for each that something else uses (`ISel::overwritten`), and a swap of a commuting node that saves such a copy is kept.
  An address form converted by `lea` adds 64-bit values without overwriting either, at the cost of an `add`, so it wins exactly where the `add` needs such a copy.
  Without liveness, a parameter or loop φ never counts as overwritten, since its other uses are mostly in other continuations (`lit/mem/seo/dont_prop_old.mim` regresses otherwise).
  The frontend rejects a rule whose type mentions one of its meta variables, so a size the type names is a family parameter: a conversion of `core.conv` takes both sizes, one instance per pair of register sizes.
- The build runs `mim` with `x86_isel` loaded as a module, so `x86` and `x86_isel` exist only if `MIM_X86` (neither `MIM_STATIC_PLUGINS` nor `MIM_NATIVE_MIM`); lit then excludes `lit/x86/` and drops the `x86_64-linux` feature.
  Generating needs `x86.mim` but not `libmim_x86`.

### Open points

- A multi-result pattern that should win only because it is shared (`sub` and `cmp` of the same operands) is underpriced by the one-rule-per-result scheme; to be revisited with flags.
- Flags: `cmp` + `cc` as a rule of type `Flags` consumed by branches and `cmovcc`; keeping the flags alive until then is the scheduler's job.
- The cost unit is latency plus one per instruction for a generic recent core; a per-µarch table, µops, or encoding size as a tie-breaker are open.
- The copy charge only sees use counts, not liveness: an operand whose other uses all come earlier is charged anyway, and a parameter used again in the same block is not.

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
axm fin: [r: I4] → {pe: «2; Nat»} → math.F pe → math.F pe;
```

- `x86.in r v` is `v`, which lives in register `r` in hardware encoding order (rax = 0, rcx = 1, rdx = 2, rbx = 3, rsp = 4, rbp = 5, rsi = 6, rdi = 7, r8 = 8, …, r15 = 15) and `x86.fin` the same for an XMM register.
  The lets `x86.reg.rax = 0I4` etc. form the module `x86.reg` and the lets `x86.freg.xmm0 = 0I4` etc. the module `x86.freg`, whose subtags are in that order; C++ takes `Num_Regs`/`Num_Fpr` and a register's encoding from them (`Annex::num<reg>()`, `encoding(reg::rax)`), so the registers are written down once.
- `I4` makes "a general-purpose register" a typing fact and the `math.F pe` of `fin` a floating-point one; the width comes from `s`, so al/ax/eax/rax need no names, and since one value occupies a whole physical register, subregisters cannot alias.
- `in` and `fin` are no instructions: they are not scheduled and sit on no chain, like `x86.addr`.
- Not wrapped: literals (immediates), `mem.M`, `Flags` (one flags register), `Spill s`/`FSpill pe` (a stack slot each, in `emit`), `Addr` itself (its base and index are wrapped inside it), and the `Bool` of a `cc` that only a branch consumes.
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

1. Every use of a value of a register class is `in r v` (general-purpose) or `fin r v` (floating-point), and all of them agree on `r`.
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
- `x86.color` builds one PBQP per function and register class (16 options, rsp and rbp ∞ in the general-purpose one and none reserved in the floating-point one): fixed operands and results (`constraint()`) are precolored; interference edges are ∞ on the diagonal, tie edges ∞ off it, jump-argument–parameter affinities cost a move off it; clobbers go into the cost vectors (`rdx` on `div`'s operands, caller-saved registers across a call); `rn_priority` is the position of the definition in an order that visits dominators first (see *Status*).
- `x86.emit` reads the registers from the `in`/`fin` map, sequentializes each jump's moves, saves the callee-saved general-purpose registers it uses, and falls through where its layout allows (see *Status*).

Each stage dumps and round-trips, so lit tests check `x86.in` and `x86.fin` directly.

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
  The others need calls to `malloc`/`free`, aggregate memory operations, unions, or a `math` operation beyond `f32`/`f64` arithmetic and conversion.
- `mim-pbqp-test` checks `solve` against `solve_exhaustive` on small instances; there is no `mim-x86-test` yet, and the register-allocation invariants are checked by `x86.color` on every compiled function instead.
- The optimizer-dump oracle for any change to the pre-isel pipeline.
