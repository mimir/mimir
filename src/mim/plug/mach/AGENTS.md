# mach

- The plugin `mach` is everything target-independent of the native backends `x86` and `arm`: the axioms `Flags`, `Spill`, `in`, `sched`, `slot`, `load`, `store`, `spill`, `reload`, the float format checks `fpe`/`fok`, the float rule types and the families `float.arith`/`float.unary`, the isel guard `lit`, and the phases `libm`, `legalize`, `schedule`, `spilling`, `constrain`, `color`, and `live`; besides `Liveness`, `ISel`, the bases `Emitter`/`AsmEmit`, `mach::Gen`, and `mach::Target`.
- Its code sees `core`, `mem`, and `mach` directly; there is no flag table of annexes in the Target.
  A backend's Target only adds its instructions (`is_machine_insn`, `is_mem_insn`, `is_removable`), its condition codes (`is_flags`, `is_sched_cc`), its register file, ABI, `constraint`, and `fits_imm`.
- Plugins never link each other, so whatever a backend derives from or instantiates is header-only, and the longer parts are compiled once into `libmim_mach` and reached via `GET_FUN_PTR`, as `ll::Emitter` does: `mim_mach_emit` (the control flow behind `Emitter::emit`, which owns the `Liveness`), `mim_mach_isel` (the factory behind `hook_isel`), and `mim_mach_gen` (behind `<backend>_isel.gen`, which needs `core`/`math`/`refly`).
  Keep `Liveness` out of the `Emitter` class: its constructor lives in `libmim_mach`, which a backend cannot link.
- `hook` constructs a phase with `Target::of(driver)`, the one loaded backend's Target; the phases read their `-X` arguments as the backend's (`-X x86:k=3`).
- `Libm` is a `mim::DS2CPS` before `mem.add_mem`: it lifts a `math` operation without an instruction into a call of a bodyless, memory-free `Cn [F…, Cn F]` named after its libm function, and `mem.add_mem` adds the `M` to it like to every Lam.
  It needs a real memory, which only `mem.add_mem` threads; `mach.schedule` maps a token to its chain end only in the Lam that defines it, so a placeholder or the enclosing Lam's `M` leaves a stale one in later continuations.
- `Legalize` is a `mim::DS2CPS`: `mem.malloc`/`mem.free` become CPS calls of bodyless `Cn`s named `malloc`/`free`, which the emitters call by symbol.
- After `legalize`, no `core.Ptr` is left: pointers are `I64` and memory goes through `load`/`store`, which isel must replace (`Target::is_lowered`).
- `Liveness` is slated for replacement; don't build further analyses on it.
- Inside a class derived from `Emitter` or `FunPhase`, a bare `target()` is the base's accessor; pass `Target::of(driver)` to the base constructor explicitly.
