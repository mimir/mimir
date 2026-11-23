# Native AArch64 Backend

This is the design and the living status of the `arm` plugin; update *Status* and *Known gaps* with every backend change.
It is the AArch64 sibling of `../x86/` and shares everything target-independent with it; see *Shared with x86*.

## Goal

A native AArch64 backend for the scalar subset of integers and `f32`/`f64` floats, enough to run the programs that `x86` runs.
Everything stays in MimIR, libFirm style: no separate machine IR.

## Decisions

### Everything stays in MimIR

- Legalize, isel, scheduling, spilling, coloring, and emission are all phases on the World; only `mem.legalize_native` and `arm.isel` change types (`RWPhase`), the others rewrite each function in place.
- Dumping, `-o` round-trip, lit tests, and the type checker work at every backend stage.

### Instruction types

- The instructions are axioms typed over `Idx s` or `math.F pe`; AArch64 is three-address, so no operand is tied to a result and `Target::constraint` stays empty.
- `mach.in r v` is `v` living in register `r` of the module `arm.reg`; it is no instruction and never on the chain.
- A literal operand stays a literal in the IR: the emitter prints it as an immediate where the encoding has one and otherwise loads it with `ldr wD, =imm`/`ldr xD, =imm` into a scratch register.
  So every hole of an `arm_isel` rule is a `Reg`, and the conversion `mov` of a literal into one is the identity at cost 0.
- `mach.slot n` reserves `n` frame bytes on the chain and yields their address; `mem.mslot` of a literal size becomes it in `mem.legalize_native`.

### The schedule is the `mem.M 0` chain

- Scheduling makes the `mem.M 0` chain total over a block's instructions; memory instructions sit on it and pure ones join it via `mem.sched`.
- Every block and function takes an `M`, and every jump and call passes the current one.

### Condition codes

- `arm.cmp` sets `NZCV` (a `mach.Flags`) from `a - b`; `arm.cc.<c>: mach.Flags → Bool` tests it and is `b<c>` as a branch condition and `cset` as a value.
- `arm.csel.<c> (flags, x, y)` yields `y` if `<c>` holds, else `x`, i.e. `(x, y)#cond`; the emitter prints the operands swapped, since `csel Rd, Rn, Rm` yields `Rn` on true.
- `arm.cc` and `arm.csel` are families in hardware encoding order (`eq ne cs cc mi pl vs vc hi ls ge lt gt le al nv`), so a condition is a subtag rather than an operand.
- `core.icmp` maps `e ne sl sle sg sge ul ule ug uge` onto `eq ne lt le gt ge cc ls hi cs`; the remaining predicates are rejected.
- A select is `csel.ne (cmp (c, 0), x, y)`; `arm.csel`'s normalizer turns it into `csel.<c> (f, x, y)` if `c` is `cc.<c> f`.

### Register assignment

- `x0`–`x7` are the argument registers, `x0` the result register, `x19`–`x28` callee-saved, `x29`/`x30` the frame and link register.
  `x16`, `x17`, and `x18` are reserved: `x17` holds a literal operand, `x16` a second one, the save that breaks a copy cycle, or a spill slot in transit.
- A spill slot is 8 bytes and holds a whole `x` register; a memory φ is a parallel copy between slots through the stack (`str x16, [sp, #-16]!`, `ldr x16, [sp], #16`), and the emitter offsets slot addresses by what it pushed.

### Floating point

- `math.F pe` values live in `d0`–`d31` as `Target::Fpr`; the format picks the `s` or `d` name of a register, so a register always holds a whole 64-bit lane.
- `d0`–`d7` are the floating-point argument registers and `d0` the result, `d8`–`d15` callee-saved (only their low 64 bits under AAPCS64, which is all a `d` save holds), and `d31` the reserved copy-cycle scratch.
- Only `math.f32` and `math.f64` are supported, checked by `mach.fpe`/`mach.fok`, which `x86` shares.
- An operand is always a register: a float literal becomes one with `fmov dD, xzr` for `+0.0`, `fmov dD, #imm` for an 8-bit immediate, and `ldr x17, =bits; fmov dD, x17` (or the `w17`/`sD` form) otherwise, since `ldr dD, =imm` is not an AArch64 instruction.
- AArch64 scalar FP has no memory operand, so no rule folds a load, and `mem.load`/`mem.store` of a float are `arm.fload`/`arm.fstore`.
- `arm.fcmp` sets `NZCV` like `arm.cmp`, and each `math.cmp` predicate is an isel rule onto one `arm.cc` (`l` is `mi`, `ug` is `hi`, …); only `ne` (`mi` or `gt`) and `ue` (`eq` or `vs`) take two and an `orr`.
- A float select is `fcsel.ne (cmp (c, 0), x, y)`, which shares `arm.csel`'s normalizer, so a select on an integer or float comparison is one compare and one `fcsel`, and a branch on one is a `b.<c>`.
- `arm.i2f`/`arm.u2f`/`arm.f2i`/`arm.f2u` take `i32` and `i64`; `i8` and `i16` go through a `w` register with `sbfx`/`ubfx` or a truncation.

### Frame

- Prologue: `stp x29, x30, [sp, #-16]!`, `mov x29, sp`, then `sub sp, sp, #frame` for the callee-saved registers (the used `x` ones, then the used `d` ones), spill slots, and `mach.slot` areas, which are addressed positively from `sp`.
- Epilogue: reload the saved registers from `[sp, #off]`, `mov sp, x29`, `ldp x29, x30, [sp], #16`, `ret`.
- Calls keep `sp` 16-byte aligned; at most eight arguments are supported.

### Shared with x86

- The plugin `mach` holds `Flags`, `Spill`, `in`, `slot`, `spill`, `reload`, the float format checks `fpe`/`fok`, the float rule types and the families `float.arith`/`float.unary`, the isel guard `lit`, and the phases `spilling`, `constrain`, `color`, and `live`, which take the target from the `Driver`; `arm.mim` only adds its registers, instructions, and condition codes.
- `mim::Target` (`include/mim/target.h`) holds the register classes, the register file and ABI per class, and `constraint`; `mach::Target` (`include/mim/plug/mach/target.h`) implements what `mach` determines, so `arm::Target` (`include/mim/plug/arm/target.h`) is just the register file, ABI, and memory instructions; `arm::target()` registers it via `Plugin::target`.
- `mem.legalize_native` and `mem.schedule` are `mem` phases that take the target from the `Driver`.
- libmim (`src/mim/native/`) holds the phases parametrized by a Target: `native::ISel` (PBQP over a generated `rules::Table`), `native::Spilling`, `native::Constrain`, `native::Color` (PBQP with its checker), `native::Live`, and `native::Emitter` (blocks, jumps, calls, returns, parallel copies, `mach.slot`/`spill`/`reload` via hooks, frame bookkeeping); the plugin `mach` holds `mach::Gen`, the rule generator behind `arm_isel.gen`.
- `mim::Liveness`, `Assign`, `layout`, and `AsmEmit` stay in libmim.
- Since `arm.isel` compiles in the table that the `mim` it builds generates, `arm` and `arm_isel` exist only if `MIM_NATIVE_BACKENDS`, like `x86`.

## Status

Done:

- `arm` plugin (`src/mim/plug/arm/`), with `ll.drop_hints`, `mem.add_mem`, `core.legalize_dyn_idx`, `mem.legalize_ptr`, `mem.legalize_native`, `arm.isel`, `mem.schedule`, `mach.spilling`, `mach.color`, and `arm.emit` at the end of the default pipeline in `opt.mim`; `mach.live` prints `Liveness` (`-X arm:live=<file>`).
- `arm.mim`: the modules `reg` (`x0`…`x30`) and `fpr` (`d0`…`d31`), `load`, `store`, `add`/`sub`/`and`/`or`/`xor`/`mul`, `madd`, `shl`/`shr`/`sar`, `udiv`/`sdiv`, `zext`/`sext`/`trunc`, `cmp`, the families `cc`, `csel`, and `fcsel`, `fmov`, `fload`/`fstore`, `fadd`/`fsub`/`fmul`/`fdiv`, `fsqrt`/`fabs`, `fcmp`, `i2f`/`u2f`/`f2i`/`f2u`/`f2f`, and the phases `isel` and `emit`.
- `arm_isel.mim` (`src/mim/plug/arm_isel/`) holds the rules: `core.wrap` (add/sub/mul/shl), `core.bit2` (and/or/xor, and nand/nor/nxor with an `eor`), `core.shr`, `madd` for a sum with a sole product, `core.div` (quotient and remainder via a `mul` and a `sub`), `core.rem` by a literal, `core.icmp`, `core.conv`, an integer `core.bitcast`, `mem.load`/`mem.store` of integers, and a select `(x, y)#c`.
  The float module converts `math`'s arithmetic, unary operations, comparisons, loads/stores, selects, and integer↔float conversions into the `arm` axioms, and `fmov` turns a float literal into a register.
  `arm.isel` is `native::ISel` over them; anything else in `core`/`mem`/`math` is an error naming the axiom.
  A rule costs the summed `arm::insn::cost` of its rhs (`include/mim/plug/arm/insn.h`).
- `mach.spilling` brings each function down to `k` registers of each class (26 integer and 31 float by default, `-X arm:k=<n>`), `mach.color` assigns them by PBQP and checks the result; see `../x86/AGENTS.md` for both.
- `arm.emit` (`emit.cpp`) writes GAS AArch64 syntax to `<world>.s` (`-X arm:o=<file>`) on top of `native::Emitter`, and tracks which result the flags hold so a clobbered condition is an error.
  Literal materialization is `ldr wD, =imm`/`ldr xD, =imm` (a GAS pseudo-instruction that clang's integrated assembler resolves through a literal pool), and a float literal is an `fmov` immediate or goes through a general-purpose register.
- Tests: `lit/arm/emit_arith.mim` (arithmetic, literals, shifts, remainder, bitcast), `lit/arm/emit_cf.mim` (a loop with a parameter swap, a branch condition, a select, a call), `lit/arm/emit_mem.mim` (slot, store, load, a pointer argument), `lit/arm/emit_float.mim` (float arithmetic on both formats, the three literal forms, conversions including `i8`/`i16`, a load/store, `fcsel` on a `Bool` and fused with an integer or float comparison), `lit/arm/emit_float_cmp.mim` (the condition code of each `math.cmp` predicate, including NaN), `lit/arm/emit_float_call.mim` (float and mixed arguments across a call, the swap through `d31`, a float in a saved `d8`–`d15` across a call), `lit/arm/isel_fold.mim` (`madd`, a shared product, `nand`), `lit/arm/spill.mim` (spills and memory φs at `k = 2`).
  They check the emitted assembly with FileCheck only.
  By hand, all 48 programs of the `x86` allowlist that link without libc (plus `atoi`) give the same exit code under `qemu-aarch64` as natively on `x86`, also at `k = 3` and `k = 2`; the float tests were run the same way under `qemu-aarch64` with a hand-written `_start`, but not yet since `fcsel`, the flag-based `fcmp`, the `fmov` immediates, the `i8`/`i16` conversions, and the callee-saved `d` registers.

Known gaps and bugs, as of 2026-11-23:

- No vectors and no aggregates, and no `mem.alloc`/`mem.malloc` lowering beyond what `mem` already does.
- Only `math.f32`/`math.f64` floats; no `f16`, no `bf16`. A float always occupies a whole `d` register, frame slot, and spill slot, whatever its format.
- Saved registers go one `str`/`ldr` each, not paired with `stp`/`ldp`.
- `ne` and `ue` rematerialize the `fcmp` per condition code, so they cost five instructions; a branch or select on them goes through a register.
- Of `math`, only `arith` without `rem`, `abs`, `rt.sq`, `cmp`, and `conv` have rules; `arith.rem`, `extrema`, `round`, `exp`, `tri`, `pow`, and the rest are isel errors.
- `i1` has no conversion to or from a float.
- `mach.slot`/`arm.load`/`arm.store` use a literal `[sp, #off]`/`[xR]` addressing mode only; there are no address-mode rules, and offsets beyond the immediate range in `load`/`store` are not handled.
- A frame larger than 4095 bytes materializes a `mach.slot` offset in `x17`, but saved registers, spill slots, and loads/stores still assume an encodable offset.
- At most eight arguments per call or entry; a `csel` with both operands literal uses both scratch registers.
- A conversion or bitcast only has rules between the register sizes `i1`, `i8`, `i16`, `i32`, `i64`.
- The scheduler leaves a dead copy of a branch's `cmp` behind (the same rematerialization as `x86`), which `mach.color`'s pruning removes.
- No lit test assembles or runs AArch64 code; the `qemu-aarch64` checks above are manual.
- `arm.reg`, `arm.fpr`, `arm.cc`, `arm.csel`, and `arm.fcsel` must stay in hardware encoding order; C++ reads registers and condition codes off the subtag and nothing checks it.

## Next Steps

1. A lit feature for `qemu-aarch64` with a minimal `_start`, so the allowlist runs on ARM in CI.
2. Address-mode rules (`[xN, #imm]`, `[xN, xM, lsl #s]`) as non-terminals, like `x86`'s `Base`/`Index`, and shifted-operand forms.
