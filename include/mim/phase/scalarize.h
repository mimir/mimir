#pragma once

#include <fe/bitset.h>

#include "mim/phase.h"

namespace mim {

/// Perform Scalarization (= Argument simplification).
/// This means that, e.g.,
/// ```
/// f := λ (x_1: [T_1, T_2], .., x_n: T_n).E
/// ```
/// will be transformed to
/// ```
/// f' := λ (y_1: T_1, y_2: T_2, .. y_n: T_n).E[x_1 \ (y_1, y_2); ..; x_n \ y_n]
/// ```
///
/// The transformation is **type-directed**:
/// the decision to flatten is made per continuation *type* (immutable `Cn`) - not per Lam.
/// Every producer and consumer of such a type is reshaped in the same sweep:
/// the Pi itself, all Lam%s of that type, and all App%s through it - no matter whether the callee is a mutable Lam,
/// a Var (higher-order parameter), a branch tuple, or a value loaded back from memory.
/// Hence, a Lam may escape (be stored, passed as argument) and still get its signature flattened.
///
/// Where a reshaped value meets a shape that is *not* reshaped, it is coerce()d:
/// tuples are exploded / reassembled and Cn%s are η-wrapped.
/// This happens wherever the expected shape stems from elsewhere:
/// an App%'s domain (in particular an Axm's signature, e.g. `affine.For`'s body), an Axm's result,
/// a Tuple's type, and the body of an *interface* Lam (external, annex, or unset declaration).
/// The latter keeps the shape of everything its signature mentions - only its own Pi is flattened.
///
/// A Pi is *pinned* (left untouched) where a coercion would be observable:
/// * a *bare* function argument of an Axm (`autodiff.ad f`) - the Axm inspects and calls it by its own convention,
/// * it types a value inside a dependently-typed aggregate (a typed closure), or
/// * one of its parameters is Extract%ed / Insert%ed via a **non-constant** index
///   (splitting would only force the body to reassemble the tuple),
///   or an Axm consumes it as a whole through one of its type variables (`tensor.buf x` nominates `x` as a tensor);
///   this is tracked per parameter via a keep-bitmask.
///
/// The phase flattens **one level** of a Pi's (thresholded) domain per run.
/// Because it is scheduled inside a fixed-point pipeline (`compile.phases tt (...)`),
/// re-running it converges to a full flatten; each run that peels calls invalidate().
///
/// Flattening respects Flags::scalarize_threshold via the thresholded projection helpers
/// (Def::num_tprojs, Pi::tdom, App::targ): an Arr parameter is only expanded if its arity is
/// below the threshold; a Sigma parameter always is.
/// It will not flatten mutable @p Sigma%s or @p Arr%ays (their vars have no static arity).
class Scalarize : public ReprPhase {
private:
    /// Optimistic fixed-point analysis: every immutable `Cn` is assumed flattenable
    /// until proven otherwise (see the pinning rules above).
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "Scalarize::Analysis") {}

        /// Per-parameter expand mask for @p type; a set bit marks a parameter to be flattened one level.
        /// An **empty** mask means "leave untouched".
        /// Cheap; recomputed on demand from the (post-fixed-point) lattice.
        fe::Bitset plan(const Def* type) const;

    private:
        const Def* rewrite(const Def* old) final;

        void inspect(const Def* def);
        /// Marks parameter @p dom of @p pi as *keep whole*; a fresh bit invalidate()s.
        void keep(const Pi* pi, size_t dom);
        /// Keeps @p def whole if it is a Lam's Var or one of its parameters.
        void keep_param(const Def* def);
        bool kept(const Pi* pi, size_t dom) const; ///< Is parameter @p dom of @p pi kept whole?

        /// Which parameters of a Pi must not be split.
        /// Kept out of lattice(), as a lattice write would also map() the Pi to its abstract value -
        /// which breaks as soon as that Pi occurs in a type argument (`mem.slot (H, 0)`).
        DefMap<fe::Bitset> keeps_;
    };

public:
    Scalarize(World& world, flags_t annex)
        : ReprPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    /// Rewrites what an interface Lam's signature mentions: it keeps its shape.
    static constexpr Ctx Iface = 1;

    const Def* rewrite_imm_Pi(const Pi*) final;
    /// The new Lam for @p old, whose Var maps to the old Var reassembled from the new one.
    Lam* stub(Lam* old, const fe::Bitset& mask);
    /// Flattens @p pi%'s domain one level according to @p mask.
    const Def* flatten(const Pi* pi, const fe::Bitset& mask);
    const Def* rewrite_mut_Lam(Lam*) final;

    Analysis analysis_;
};

} // namespace mim
