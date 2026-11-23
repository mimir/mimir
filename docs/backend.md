# Native Backends {#backend}

[TOC]

MimIR has two native backends besides the LLVM-emitting [`ll`](@ref ll): [`x86`](@ref x86) for x86-64 and [`arm`](@ref arm) for AArch64.
Both translate a MimIR program directly into GNU assembler text, which `clang` or `as` assembles.
This page explains how they work; the living design notes and status of each backend are `src/mim/plug/x86/AGENTS.md` and `src/mim/plug/arm/AGENTS.md`.

## Design in a Nutshell {#backend_design}

- **Everything stays in MimIR.**
  There is no separate machine IR: instruction selection, scheduling, spilling, register assignment, and SSA destruction are all [phases](@ref phases) on the [`World`](@ref mim::World), in the style of libFirm.
  Machine instructions are ordinary axioms, so dumping, `-o` round-trips, lit tests, and the type checker work at every backend stage.
- **SSA until the very end.**
  Register allocation runs on SSA form (Hack and Braun style); SSA destruction happens only in the emitter, which turns each jump into a parallel copy.
- **PBQP twice.**
  Both instruction selection and register assignment are posed as partitioned boolean quadratic problems and solved by the in-tree solver `libpbqp` (`src/pbqp/`).
- **Rules are data.**
  The instruction-selection rules are written in Mim, as `rule`s in the plugins `x86_isel` and `arm_isel`, and compiled into C++ tables at build time.
- **No CFG in the IR.**
  The schedule is an explicit edge in the graph: the `mem.M` chain.
  Liveness, dominators, and loops are side tables computed per function by the phases that need them, never stored in the IR.

## Usage {#backend_usage}

Load the backend plugin and name the output file:

```sh
mim -p opt foo.mim -p x86 -X x86:o=foo.s && clang foo.s -o foo
mim -p opt foo.mim -p arm -X arm:o=foo.s && clang foo.s -o foo
```

Loading the plugin appends the backend's phases to the default pipeline (see `opt.mim`); without `-X <backend>:o=`, the output goes to `<world>.s`.
Since both backends register the same target-independent machinery, loading `x86` together with `arm` is an error.

| Argument | Meaning |
| -------- | ------- |
| `-X x86:o=<file>`, `-X arm:o=<file>` | Assembly output; `-` is stdout. |
| `-X x86:k=<n>`, `-X arm:k=<n>` | Number of general-purpose registers the spiller may use (default 14 on x86, 26 on AArch64); a small `k` stress-tests spilling. |
| `-X <backend>:live=<file>` | Where `mach.live` prints its liveness tables. |
| `-X <backend>:color-dot=<file>` | The register-assignment PBQP of each function as Graphviz DOT, with the chosen options highlighted. |
| `-X x86:isel-dot=<file>` | The instruction-selection PBQP as Graphviz DOT. |

The full lists are generated from the plugins: [`-X x86:<arg>`](@ref xarg_x86) and [`-X arm:<arg>`](@ref xarg_arm).

The backends exist only if the build can run the `mim` it builds (`MIM_NATIVE_BACKENDS`), because generating the rule tables needs it; static builds (`MIM_STATIC_PLUGINS`) and cross builds (`MIM_NATIVE_MIM`) have none.

### Example

```mim
plugin core;
plugin mem;

use core.ops.u.w;

extern fun f(a b: I64): I64 = return ((a + b) * 3I64);
```

`-p x86` yields

```
f:
	pushq %rbp
	movq %rsp, %rbp
	movq %rdi, %rax
	addq %rsi, %rax
	imulq $3, %rax
	leave
	ret
```

and `-p arm`

```
f:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	add x0, x0, x1
	ldr x17, =3
	mul x0, x17, x0
	mov sp, x29
	ldp x29, x30, [sp], #16
	ret
```

Right before emission, the x86 World reads (with `-o -`, abridged and with binders renamed):

```mim
extern con f (m: mem.M 0, a b: I64, ret: Cn [mem.M 0, I64]) =
    let a_rax = mach.in 0I4 a;
    let b_rsi = mach.in 6I4 b;
    let x     = mem.sched (x86.add @ 0) m (a_rax, b_rsi);
    let x_rax = mach.in 0I4 x#tt;
    let y     = mem.sched (x86.imul @ 0) x#ff (x_rax, 3I64);
    ret (y#ff, mach.in 0I4 y#tt);
```

Every instruction sits on the `mem.M` chain via `mem.sched`, and every use of a value states its register with `mach.in`.
The entry parameter `a` arrives in `rdi` but lives in `rax`, so the emitter copies it there; the return is the copy `rax → rax`, which vanishes.

## Code Organization {#backend_code}

| Where | What |
| ----- | ---- |
| `src/mim/native/`, `include/mim/native/` | The target-independent phases in libmim: `native::ISel`, `native::Spilling`, `native::Constrain`, `native::Color`, `native::Live`, and `native::Emitter`, all parametrized by a `mim::Target`. |
| `include/mim/target.h` | `mim::Target`: register classes, the register file and ABI per class, and fixed-register constraints. |
| `src/mim/plug/mach/` | The machine-independent axioms (`mach.Flags`, `mach.Spill`, `mach.in`, `mach.slot`, `mach.spill`/`reload`, the float format checks `fpe`/`fok`), the phases `mach.spilling`, `mach.constrain`, `mach.color`, `mach.live`, the rule generator `mach::Gen`, and `mach::Target`. |
| `src/mim/plug/x86/`, `src/mim/plug/arm/` | Registers, instructions, and condition codes as axioms; the target description (`target.h`: memory instructions, register file, ABI, constraints); instruction costs (`insn.h`); the phases `<backend>.isel` and `<backend>.emit`. |
| `src/mim/plug/x86_isel/`, `src/mim/plug/arm_isel/` | The instruction-selection rules in Mim and the phase `<backend>_isel.gen`, which writes them as C++ tables into `build/include/mim/plug/<backend>/isel_rules.h`. |
| `src/mim/plug/mem/phase/` | `mem.add_mem`, `mem.legalize_ptr`, `mem.legalize_native`, and `mem.schedule`, which take the target from the [`Driver`](@ref mim::Driver). |
| `src/pbqp/` | The PBQP solver with the RN heuristic and a `rn_priority` hook. |

A backend plugin is thin: it registers its `Target` via `Plugin::target` and supplies its instructions, its rules, and the textual form of its instructions; the rest is shared.

## Pipeline {#backend_pipeline}

The backend phases run at the end of the default pipeline, after the usual optimizations and closure conversion:

| Phase | Kind | Purpose |
| ----- | ---- | ------- |
| `ll.drop_hints` | rewrite | Removes the loop hints `ll.vec`/`ll.novec`. |
| `mem.add_mem` | rewrite | Gives every continuation a `mem.M`, so every block can carry the schedule. |
| `core.legalize_dyn_idx` | rewrite | Turns each `Idx n` with a dynamic `n` into `I64`. |
| `mem.legalize_ptr` | rewrite | Makes every pointer a byte pointer and every `mem.lea` a `mem.ptradd` with the target's layout. |
| `mem.legalize_native` | rewrite | Turns every `Nat` value and every pointer into `I64` and every literal-sized `mem.mslot` into `mach.slot`, so isel only sees register types. |
| `<backend>.isel` | `RWPhase` | Covers `core`/`mem`/`math` operations with machine instructions by PBQP. |
| `mem.schedule` | in place | Places every instruction and threads it onto its block's `mem.M` chain. |
| `mach.spilling` | in place | Brings register pressure down to `k` per register class. |
| `mach.constrain` | in place | Splits live ranges around fixed-register and two-address instructions (x86 only; AArch64 has none). |
| `mach.color` | in place | Assigns registers by PBQP and checks the result. |
| `<backend>.emit` | output | Writes the assembly. |

Only the legalizations and isel change types; the later phases rewrite each function in place and create a new continuation only where a block's signature changes.

## Instructions as Axioms {#backend_insns}

A machine instruction is an axiom typed over plain integers and floats, e.g.

```mim
axm (add, sub): {s: Nat} → «2; Idx s» → Idx (gpr s); // in x86.mim
```

- Registers and the schedule are no concern of an instruction's type.
- The operand sizes an instruction supports are a `refly.check` in its codomain (`gpr`, `gpr16`, `logic`, `mach.fpe`), so a literal size folds the check away and an unsupported one is a type error.
- Condition codes are families in hardware encoding order (`x86.cc.l`, `arm.csel.ne`), so a condition is a subtag, not an operand.
  `x86.cc.l: mach.Flags → Bool` is `setl` as a value and `jl` as a branch condition.
- A literal operand stays a literal.
  On x86 it is an immediate if it fits and otherwise goes through `x86.mov`, a choice isel makes per use; on AArch64 the emitter materializes any literal that has no immediate form into a scratch register.
- x86 addresses are operands of type `x86.Addr i`, built by `x86.addr (disp, base)` and `x86.index a (index, scale)`.
  Whether an address has an index is part of its type (`Base = Addr ff`, `Index = Addr tt`), so "at most one index register" is a typing fact.
  An address is no value: it is not scheduled and occupies no register of its own; `x86.lea` turns it into one.
- An instruction with several results is tuple-valued: `x86.div` yields quotient and remainder as a pair, so a division and a remainder of the same operands share one instruction.

## Instruction Selection {#backend_isel}

Instruction selection follows Buchwald & Zwinkau, *Instruction Selection by Graph Transformation* (CASES 2010): the program graph is covered by matches of rewrite rules, and the choice of cover is a PBQP.
MimIR generalizes the holes of a pattern from "a value in a register" to typed terms, which factors the grammar (Ebner et al., LCTES 2008).

### Rules

A rule rewrites a `core`/`mem`/`math` pattern into machine instructions:

```mim
rule load  {s: Nat} (m: mem.M 0, p: mem.Ptr0 (Idx s)): mem.load (m, p) => x86.load s (m, addr p);
rule icmp  (p: …, c: mach.Flags → Bool) (s: Nat, a b: Idx s): p (a, b) => c (x86.cmp (a, reg_imm b));
rule rm    … (k: Nat, a: Idx s, m: mem.M 0, p: mem.Ptr0 (Idx s)):
    let (mm, v) = mem.load (m, p); (mm, op k (a, v)) when x86.is_gpr s => x86.rm insn (m, a, addr p);
rule lea   (i: Bool) (a: x86.Addr i): a => x86.lea a;
```

- The meta variables are the holes.
  Each hole has a *want*, derived from its type and markers: a register (`Reg`, `FReg`), an address (`Base`, `Index`, `Addr`), a scaled index, an immediate (`reg_imm` lets a literal that fits become `Imm`), or a literal (`mach.lit`).
  The type of the replacement is the rule's *non-terminal*.
- A pattern may cover several nodes: `rm` folds a load into an arithmetic instruction (`addq (%rdi), %rax`), and `mr` folds a load-operate-store into one instruction.
  Inner nodes must be *sole*, i.e., have no user outside the match, and a covered memory operation must have nothing in between.
- A rule whose pattern is a single hole is a *conversion* between non-terminals: `lea` makes a register of an address, `base` an address of a register, `mov` a register of a literal.
  Isel chains them at compile time and prices each hole by the cheapest chain.
- Families are instantiated at every operand size (`x86_isel.sizes`) or float format (`x86_isel.formats`) that the `refly.check`s of the instructions admit; a `when` guard decides what the types cannot, such as whether a conversion narrows or widens.
- A rule costs the summed `insn::cost` of its replacement: each instruction's latency on recent cores plus one, so of two equally fast covers the shorter one wins.

At build time, `<backend>_isel.gen` (`mach::Gen`) reads these rules and emits one C++ matcher per distinct pattern, one builder per distinct replacement, and one table row per rule with its cost, root axiom, non-terminal, and wants.
It also checks the rule set for solvability and fails the build naming the offending rule:

- *Atomic completeness*: every operation has a rule without inner nodes whose holes every non-terminal converts into, which is always a valid fallback cover.
- *Total conversions*: every non-terminal converts to a register, and every hole type is reachable.

### The PBQP

`native::ISel` builds one PBQP for the whole World:

- Every operation with a matching rule is a node; Vars and literals are not.
- A node's alternatives are one *root* alternative per rule rooted at it and one *inner* alternative per class of matches that cover it as an inner node.
  A rule's cost sits on its root alternative.
- An edge from a user to an operand prices the conversion from the operand's chosen non-terminal to the user's want, ∞ if there is none; an inner alternative must agree with the match that covers it.
- Conversions live on edges, so each user converts for itself: one shared address can fold into a load and be materialized by `lea` for a call.
- A two-address instruction (x86) charges a copy for each overwritten operand that something else uses, so the solver prefers a `lea` where an `add` would need a copy.

After solving, the rewrite builds each root alternative's replacement with its holes substituted by the rewritten and converted operands; any `core`/`mem`/`math` operation left over is an error naming the axiom.

## Scheduling {#backend_sched}

There is no list of instructions per block.
Instead, the schedule is the `mem.M` chain: after `mem.schedule`, every instruction of a block sits on that block's chain, memory instructions directly and pure ones via

```mim
axm sched: {D C: *} → (D → C) → M 0 → D → [M 0, C]; // in mem.mim
```

e.g. `let (m, x) = mem.sched x86.add m (a, b)`.

- Each instruction is placed once, as late as possible but out of loops, and the chain follows a topological order.
- The chain token makes each placement a distinct Def, so copies, spills, reloads, and rematerializations at different program points never merge under hash-consing.
- A closed instruction (only literal operands) is rematerialized right before each use instead of being placed once.
- Flags are no register value: each consumer of a `cmp`/`test` gets its own copy right before it, so at most one flags value is live and nothing clobbers it before its consumer.
- Every block and function takes an `M`, and every jump and call passes the current one; a continuation without one is an error.

## Register Allocation {#backend_ra}

Allocation is SSA-based: spill until the register pressure fits, then assign registers to the still chordal interference graph, then destroy SSA while emitting.

### Liveness

`mim::Liveness` is a side table over one scheduled function: its blocks with their chains, predecessors and successors, live-in and live-out sets, and the maximal pressure (MaxLive) per register class.
A value's class comes from its type: `math.F pe` is floating-point, `mach.Spill T` a stack slot, `mach.Flags` the flags, everything else general-purpose.
A block parameter is defined at the block's entry and a jump uses its arguments, so parameters are the φs.

### Spilling

`mach.spilling` implements Braun & Hack, *Register Spilling and Live-Range Splitting for SSA-Form Programs* (CC 2009), once per register class:

- Global next-use distances, with a large penalty across a loop exit, decide which values to evict.
- At a block's entry, the values in registers at the end of all predecessors come first; at a loop header, the values used in the loop.
- Per instruction, missing operands are reloaded and values are evicted until the operands, the results, and the fixed-register copies of the instruction fit into `k`.
- A call keeps only callee-saved registers' worth of values live across it.
- A spilled block parameter becomes a *memory φ* of type `mach.Spill T`.
- `mach.spill` and `mach.reload` are pure instructions on the chain; a closed instruction is recomputed instead of reloaded.
- A reload gives a value a second definition; SSA reconstruction (Braun et al., *Simple and Efficient Construction of Static Single Assignment Form*, CC 2013) adds the necessary block parameters, so each block whose signature changes is rebuilt once.

### Constraints

`mach.constrain` (x86 only) isolates fixed registers and two-address forms by live-range splits:

- Right before an instruction with a fixed operand (`div`'s dividend in `rax`, a shift count in `cl`) or a tied operand that stays live, the block jumps to a fresh continuation that takes these operands.
- Right after an instruction with fixed results (`div`'s quotient in `rax` and remainder in `rdx`), the live ones go to a fresh continuation.

So a precolored live range ends at once and the interference graph stays chordal.
There is no copy instruction: a live-range split is a jump, and a jump is a parallel copy.

### Coloring

`mach.color` builds one PBQP per function and register class, following Buchwald, Zwinkau, Bersch, *SSA-based Register Allocation with PBQP* (CC 2011):

- Each value is a node with one option per register; reserved registers (`rsp`, `rbp`, `x16`–`x18`, `x29`, `x30`) cost ∞, callee-saved ones a little.
- Interfering values get ∞ on the diagonal, a two-address result and its tied operand ∞ off it.
- Fixed operands and results are precolored, clobbers and values live across a call cost ∞ on the affected registers.
- Copies are preferences: a jump argument and its parameter, an entry parameter and its ABI argument register, a returned value and the result register.
- The RN heuristic eliminates nodes in definition order of a dominator-first block order, a perfect elimination order of the chordal graph, so it never fails for unconstrained values.

The result lives in the graph: every use of a value `v` in register `r` becomes `mach.in r v`.
All uses of one value share one hash-consed `mach.in`, since each value is a distinct Def.
`mach.in` is no instruction and sits on no chain; literals, memory tokens, flags, and spill slots are not wrapped.

A checker then recomputes `Liveness` and verifies the result: every use is wrapped with one agreeing register, simultaneously live values differ, ties and fixed registers hold, nothing live across a call is in a caller-saved register, and no instruction is off the chain.
Any violation is an error naming the values.

## Emission {#backend_emit}

`native::Emitter` handles everything that is not an instruction, and `x86.emit`/`arm.emit` print the instructions:

- **Blocks** are laid out by chasing each block's last jump, so a block falls through to its successor where possible.
- **Jumps, calls, returns, and the entry are parallel copies**, which is where SSA destruction happens: spill slots first, then register moves in dependency order, breaking cycles with `xchg` (x86) or a scratch register (AArch64), then immediates.
  A call copies its arguments into the ABI registers, and its return continuation's parameter out of the result register.
- **The frame** holds the callee-saved registers in use, one slot per `mach.Spill` value, and the `mach.slot` areas, keeping the stack 16-byte aligned at calls.
- **Flags** are emitted where `mem.schedule` placed the comparison; the emitter tracks which result the flags hold and rejects a consumer whose flags something clobbered.
- **Float literals** come from a `.rodata` constant pool (x86) or an `fmov` immediate or a general-purpose register (AArch64).

## x86-64 and AArch64 Compared {#backend_compare}

| | `x86` | `arm` |
| - | ----- | ----- |
| ABI | System V | AAPCS64; Mach-O syntax on Apple hosts |
| Assembly | GAS, AT&T syntax | GAS |
| Registers | 14 general-purpose (`rsp`, `rbp` reserved), 16 XMM, all caller-saved | 26 general-purpose (`x16`–`x18`, `x29`, `x30` reserved), `d0`–`d30` (`d31` reserved) |
| Operand form | two-address, so `mach.constrain` ties results | three-address, so no constraints |
| Immediates | chosen by isel per use (`Imm`, `x86.mov`) | materialized by the emitter (`ldr x17, =imm`) |
| Address modes | `Base`/`Index` non-terminals, folded loads (`rm`, `mr`) | `[xN]` and `[sp, #off]` only |
| Fixed registers | `div`/`idiv` (`rax`, `rdx`), shift counts (`cl`) | none |
| Conditions | `cmp`/`test` + `setcc`/`jcc`/`cmovcc` | `cmp`/`fcmp` + `cset`/`b.cc`/`csel`/`fcsel` |

Both support integers up to 64 bits and `f32`/`f64` floats.
Vectors, aggregates, the transcendental `math` functions, `malloc`/`free`, more than six (x86) or eight (AArch64) call arguments, and indirect calls are not supported yet; see the *Known gaps* in each backend's `AGENTS.md`.

## Testing and Debugging {#backend_testing}

- `lit/x86/` and `lit/arm/` test each stage, mostly with FileCheck over the assembly; the x86 tests also assemble and run the result on an x86-64 Linux host.
- Integer and float programs elsewhere in `lit/` that run under `-p ll` also run under the host's native backend, via `%if native %{ … -p %native … %}` RUN lines; see `lit/lit.cfg.py`.
- [`scripts/native_ksweep.sh`](@ref native_ksweep) reruns that allowlist at small register counts `k`, which exercises spilling, memory φs, and parallel-copy cycles.
- Each stage dumps and round-trips, so `-o -` shows the IR after the backend: `mem.sched` chains, `mach.spill`/`reload`, and `mach.in` registers.
  To stop after a stage, write a local `_compile` with just the phases you need.
- `-X x86:isel-dot=` and `-X <backend>:color-dot=` show the PBQPs and the chosen options; `mach.live` prints liveness.
- `mim-pbqp-test` checks the solver against exhaustive search on small instances.

## References {#backend_refs}

- Buchwald, Zwinkau: *Instruction Selection by Graph Transformation*, CASES 2010.
- Ebner, Brandner, Scholz, Krall, Wiedermann, Kadlec: *Generalized Instruction Selection using SSA-Graphs*, LCTES 2008.
- Braun, Hack: *Register Spilling and Live-Range Splitting for SSA-Form Programs*, CC 2009.
- Buchwald, Zwinkau, Bersch: *SSA-based Register Allocation with PBQP*, CC 2011.
- Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau: *Simple and Efficient Construction of Static Single Assignment Form*, CC 2013.
- Hack: *Register Allocation for Programs in SSA Form*, PhD thesis, 2007.
