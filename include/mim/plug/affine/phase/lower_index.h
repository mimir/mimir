#pragma once

#include <cstdint>

#include <optional>

#include <mim/phase.h>

namespace mim::plug::affine::phase {

/// Lowers the affine index algebra to core arithmetic.
///
/// The opaque affine.Index type is rewritten to the wide `Idx 0` (i64) carrier, and the index operations are computed
/// with wrap-around (`core.Mode::none`) arithmetic, so negation/subtraction are correct via two's complement:
/// * `affine.lit n`           ↦ `n` reinterpreted as an `Idx 0`,
/// * `affine.op.add (a, b)`        ↦ `core.wrap.add none (a, b)`,
/// * `affine.op.sub (a, b)`        ↦ `core.wrap.sub none (a, b)`,
/// * `affine.op.neg a`             ↦ `core.wrap.sub none (0, a)`,
/// * `affine.semiop.mul (a, c)`    ↦ `core.wrap.mul none (a, c)`.
///
/// The `affine.map f idxs mem` bridge lowers to: widen each runtime `idxs#i : Idx (sin#i)` to `Idx 0` via
/// `core.conv.u`, apply the (now core-arithmetic) function `f`, and
/// narrow each result back to its target `Idx (sout#j)` via `core.conv.u`.
///
/// The division-based semiops (`ceildiv`, `floordiv`, `mod`) lower to `core.div`, which threads a `mem.M`. The mem is
/// taken from the enclosing `affine.map`'s mem operand (tracked in #mem_), advanced through the div chain, and
/// returned alongside the result.
class LowerIndex : public RWPhase {
public:
    LowerIndex(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;

private:
    /// Inclusive range of the values a lowered `Idx 0` expression can take, proven not to wrap.
    struct Range {
        int64_t lo, hi;
    };

    /// The body sees every index as a bare `Idx 0`, so this is the only place the domain is still known.
    struct Domain {
        const Def* idxs = nullptr;
        fe::Vector<std::optional<int64_t>> extents;
    };

    /// The addends of an expression that are multiples of `c`, and whether the rest can carry into a division by it.
    struct Split {
        DefVec even, rest;
        bool carries;
    };

    std::optional<Range> range_of(const Def*) const;
    Split split(const Def*, int64_t);
    const Def* fold_udiv(const Def*, int64_t);
    const Def* fold_urem(const Def*, int64_t);

    const Def* mem_ = nullptr;
    Domain dom_;

    /// Rewritten access-map lams, keyed by `f` and then `sin`: shared annex lams like `tensor.proj_map` are reached
    /// from `affine.map`s over different domains, and a fold only holds for the extents that justified it.
    DefMap<DefMap<Lam*>> specialized_;
};

} // namespace mim::plug::affine::phase
