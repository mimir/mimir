# core

- Sum types lower in `opt.mim` as `nom_erasure → single_erasure → core.lower_dyn_extract → core.lower_variant → core.legalize_idx`.
- `core.lower_dyn_extract` turns a dynamic Extract type `(Ts)#i` into an untagged `core.Union` of `Ts`.
- `core.lower_variant` picks a layout per Variant: *Finite* `Idx Σsᵢ` when all cases are `Idx`/unit, *Word* `I64` pointer tagging when the pointee alignment leaves enough low bits, else *Tagged* `[Idx n, payload]` with an opaque `core.Union` payload.
  It needs literal `trait` values and falls back to Tagged otherwise.
- Recursive (mutable) Variants like `List` are not lowered yet; they need a boxed payload.
- `core.legalize_idx` widens every literal `Idx s` that is no `Bool`/`I8`/`I16`/`I32`/`I64`; `core::isa_odd_idx_op` is the one definition of "depends on the odd modulus".
- `core.trait` still hard-codes x86-64; a target description in the `Driver` is the parked follow-up.
- `core.idx2nat` is the always-safe `Idx`→`Nat` conversion; a nonzero literal of `Idx n` with symbolic `n` is `core.conv.u n 1I64`.
