# core

- Sum types lower in `opt.mim` as `nom_erasure → single_erasure → core.lower_dyn_extract → core.lower_variant → core.legalize_idx`.
- `core.lower_dyn_extract` turns a dynamic Extract type `(Ts)#i` into an untagged `core.Union` of `Ts`.
- `core.lower_variant` picks a layout per Variant: *Finite* `Idx Σsᵢ` when all cases are `Idx`/unit, *Word* `I64` pointer tagging when the pointee alignment leaves enough low bits, else *Tagged* `[Idx n, payload]` with an opaque `core.Union` payload.
  It needs literal `trait` values and falls back to Tagged otherwise.
- Recursive (mutable) Variants like `List` are not lowered yet; they need a boxed payload.
- `core.legalize_idx` widens every literal `Idx s` that is no `Bool`/`I8`/`I16`/`I32`/`I64`; `core::isa_odd_idx_op` is the one definition of "depends on the odd modulus".
- `core.legalize_dyn_idx` (same class, other annex) turns a dynamic `Idx n` into `I64` and nothing else: rerunning the odd-size logic would take `legalize_idx`'s own conversions back to `Idx s` for real ones and add modulos.
  It cannot run where `legalize_idx` does, since the final `optimize` still specializes dependent functions (`f (n: Nat): Idx n` called with `i32`), so `opt.mim` runs it for the native backends `x86` and `arm` only, after `ll.emit`.
- `core.trait` takes the pointer size from `Driver::isa_target`, else a 64-bit host (`ll`); a `Nat` is 8 bytes everywhere, since every backend lowers it to `I64`.
  It normalizes on construction, so the backend plugin must be loaded before the code that asks, e.g. via `-p x86`.
- `core.idx2nat` is the always-safe `Idx`→`Nat` conversion; a nonzero literal of `Idx n` with symbolic `n` is `core.conv.u n 1I64`.
