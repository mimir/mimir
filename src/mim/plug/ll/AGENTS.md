# ll

- Runtime-size packs and extracts with a non-literal arity are a known limitation, not a bug.
- `Idx n` with non-literal `n` is `i64` via the ll-local `idx_bitwidth`, not `Idx::size2bitwidth`, where `trait.size` would fold too early.
- A crash under `-p opt -p ll` is first a wrong plugin set (the test needs `-p clos` or `-p cps`), not a missing backend case.
- `core.Union` prints as `[size/align x i(8·align)]` from `core.trait`; an aggregate `core.bitcast` goes through an entry-block stack slot.
- `ll` rejects whatever `core::isa_odd_idx_op` still matches after `core.legalize_idx`.
