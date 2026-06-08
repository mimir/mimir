#pragma once

#include <mim/phase.h>

namespace mim::plug::affine::phase {

/// Lowers the affine index algebra to %core arithmetic.
///
/// The opaque %affine.index type is rewritten to the wide `Idx 0` (i64) carrier, and the index operations are computed with
/// wrap-around (`%core.Mode::none`) arithmetic, so negation/subtraction are correct via two's complement:
/// * `%affine.constant n`           ↦ `n` reinterpreted as an `Idx 0`,
/// * `%affine.op.add (a, b)`        ↦ `%core.wrap.add none (a, b)`,
/// * `%affine.op.sub (a, b)`        ↦ `%core.wrap.sub none (a, b)`,
/// * `%affine.op.neg a`             ↦ `%core.wrap.sub none (0, a)`,
/// * `%affine.semiop.mul (a, c)`    ↦ `%core.wrap.mul none (a, c)`.
///
/// The `%affine.map f idxs` bridge lowers to: widen each runtime `idxs#i : Idx (sin#i)` to `Idx 0` via `%core.conv.u`, apply
/// the (now core-arithmetic) function `f`, and narrow each result back to its target `Idx (sout#j)` via `%core.conv.u`.
///
/// The division-based semiops (`ceildiv`, `floordiv`, `mod`) are not yet lowered (they require `%core.div`, which threads
/// a `%mem.M`); they currently raise an error.
class LowerIndex : public RWPhase {
public:
    LowerIndex(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::affine::phase
