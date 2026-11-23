# Native AArch64 Backend

This is the design and the living status of the `arm` plugin; update *Status* and *Known gaps* with every backend change.
It is the AArch64 sibling of `../x86/` and shares the target-independent pieces with it; see *Shared with x86*.

## Goal

A native AArch64 backend for a small integer subset, enough to run simple programs.
Unlike `x86`, instruction selection is a hand-written C++ selector instead of a generated PBQP rule table, and there is no spilling in the first version: a program that needs more registers than exist is an error.
Everything stays in MimIR, libFirm style: no separate machine IR.

## Decisions

### Everything stays in MimIR

- Legalize, isel, scheduling, coloring, and emission are all phases on the World; only `mem.legalize_native` and `arm.isel` change types (`RWPhase`), the others rewrite each function in place.
- Dumping, `-o` round-trip, lit tests, and the type checker work at every backend stage.
- See *Shared with x86* for what lives outside this plugin.

### Instruction types are plain integers

- The instructions are axioms typed over `Idx s`; AArch64 is three-address, so no operand is tied to a result and isel never needs a copy for a two-address form.
- `arm.in r v` is `v` living in register `r`; it is no instruction and never on the chain.
- A literal operand stays a literal in the IR: the emitter prints it as an immediate where the encoding has one and otherwise loads it with `ldr wD, =imm`/`ldr xD, =imm` into a scratch register.
- `arm.slot n` reserves `n` frame bytes on the chain and yields their address; `mem.mslot` of a literal size becomes it in `mem.legalize_native`.

### The schedule is the `mem.M 0` chain

- Scheduling makes the `mem.M 0` chain total over a block's instructions; memory instructions sit on it and pure ones join it via `mem.sched`.
- Every block and function takes an `M`, and every jump and call passes the current one.

### Condition codes

- `arm.cmp` sets `NZCV` from `a - b`; `arm.cc.<c>: Flags → Bool` tests it and is `b<c>` as a branch condition and `cset` as a value.
- `arm.csel.<c> (flags, x, y)` yields `y` if `<c>` holds, else `x`, i.e. `(x, y)#cond`; the emitter prints the operands swapped, since `csel Rd, Rn, Rm` yields `Rn` on true.
- `arm.cc` and `arm.csel` are families in hardware encoding order (`eq ne cs cc mi pl vs vc hi ls ge lt gt le al nv`), so a condition is a subtag rather than an operand.
- `core.icmp` maps `e ne sl sle sg sge ul ule ug uge` onto `eq ne lt le gt ge cc ls hi cs`; the remaining predicates are rejected.

### Register assignment

- `arm.color` is greedy graph coloring over `mim::Liveness`: values are colored caller-saved first and callee-saved only when live across a call; the rewrite wraps every use in `arm.in r v`.
- There is no spilling and no constraint phase: an uncolorable function is an error.
- `x0`–`x7` are the argument registers, `x0` the result register, `x19`–`x28` callee-saved, `x29`/`x30` the frame and link register.
  `x16`, `x17`, and `x18` are reserved: `x17` holds a literal operand, `x16` a second one or the save that breaks a copy cycle.

### Frame

- Prologue: `stp x29, x30, [sp, #-16]!`, `mov x29, sp`, then `sub sp, sp, #frame` for the callee-saved registers and slots, which are addressed positively from `sp`.
- Epilogue: reload the saved registers from `[sp, #off]`, `mov sp, x29`, `ldp x29, x30, [sp], #16`, `ret`.
- Calls keep `sp` 16-byte aligned; at most eight arguments are supported.

### Shared with x86

- `mim::Target` (`include/mim/target.h`) holds the axioms `core.M`, `mem.sched`, and the backend's `slot`, `cc`, and `Flags`, plus five virtual machine facts: register class, machine instruction, memory instruction, and the register operand `in`.
  `arm::target()` registers it via `Plugin::target`; `Driver::target` yields it and fails unless exactly one native backend is loaded.
- `mem.legalize_native` and `mem.schedule` are `mem` phases that take the target from the `Driver`; `mem.sched` is the shared placement axiom.
- `mim::Liveness` (`src/mim/liveness.cpp`) with `interferences`, `call_cont`, `Assign`, and the block `layout`; `mim::AsmEmit` (`include/mim/asm.h`) is the `emit` phase driver (output file, function queue, `.text`, `.note.GNU-stack`).
- The ABI stays in each plugin; the shared code never needs it.

## Status

Done:

- `arm` plugin (`src/mim/plug/arm/`), with `ll.drop_hints`, `mem.add_mem`, `core.legalize_dyn_idx`, `mem.legalize_ptr`, `mem.legalize_native`, `arm.isel`, `mem.schedule`, `arm.color`, and `arm.emit` at the end of the default pipeline in `opt.mim`.
- `arm.mim`: `Flags`, `sched`, `in` with the module `reg` of `x0`…`x30`, `load`, `store`, `slot`, `add`/`sub`/`and`/`or`/`xor`/`mul`, `shl`/`shr`/`sar`, `udiv`/`sdiv`, `zext`/`sext`/`trunc`, `cmp`, the families `cc` and `csel`, and the phases.
- `arm.isel` covers `core.wrap` (add/sub/mul/shl), `core.bit2` (and/or/xor), `core.shr`, `core.div` (quotient and remainder via a `mul` and a `sub`), `core.rem` (unsigned remainder by a literal as `udiv`+`mul`+`sub`), `core.icmp`, `core.conv`, an integer `core.bitcast` (as `trunc`/`zext`), `mem.load`/`mem.store` of integers, and a select `(x, y)#c` (as `csel`).
  Everything else in `core`/`mem` is an error naming the axiom.
- `arm.color` (`phase/color.cpp`) builds `mim::Liveness`, colors greedily, and rewrites each use; jumps, calls, returns, and the entry are parallel copies (`x16` breaks a cycle).
- `arm.emit` (`emit.cpp`) writes GAS AArch64 syntax to `<world>.s` (`-X arm:o=<file>`), lays blocks out by falling through along each terminator, saves the callee-saved registers that hold values, and tracks which result the flags hold so a clobbered condition is an error.
  Literal materialization is `ldr wD, =imm`/`ldr xD, =imm` (a GAS pseudo-instruction that clang's integrated assembler resolves through a literal pool).
- Tests: `lit/arm/emit_arith.mim` (arithmetic, literals, shifts, remainder, bitcast), `lit/arm/emit_cf.mim` (a loop with a parameter swap, a branch condition, a select, a call), `lit/arm/emit_mem.mim` (slot, store, load, a pointer argument).
  They check the emitted assembly with FileCheck only; no test assembles or runs AArch64 code.

Known gaps and bugs, as of 2026-11-23:

- No spilling: `arm.color` bails when a value has no free register.
- No floating point, no vectors, no aggregates, and no `mem.alloc`/`mem.malloc` lowering beyond what `mem` already does.
- `arm.slot`/`arm.load`/`arm.store` use a literal `[sp, #off]`/`[xR]` addressing mode only; offsets beyond the immediate range in `load`/`store` are not handled.
- A frame larger than 4095 bytes materializes the slot offset in `x17`, but saved registers and loads/stores still assume an encodable offset.
- At most eight arguments per call or entry; a `csel` with both operands literal uses both scratch registers.
- The scheduler leaves a dead copy of a branch's `cmp` behind (the same rematerialization as `x86`), so a branch usually shows two identical `cmp`s.
- No test pins the literal-pool or parallel-copy paths directly; only the emit tests exercise them.
- `arm.reg`, `arm.cc`, and `arm.csel` must stay in hardware encoding order; C++ reads registers and condition codes off the subtag and nothing checks it.

## Next Steps

1. Spilling, so programs with high register pressure compile; the `mim::Liveness` pressure and `x86`'s spill/constrain machinery are the template.
2. The integer forms the selector currently rejects (e.g. `Idx` sizes that are no multiple of 8).
3. Floating point on `d0`–`d31` as `Target::Fpr`, with `arm.in` taking the class like `x86.in`/`x86.fin`.
