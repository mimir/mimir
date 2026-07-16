#pragma once

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
/// if `f` appears in callee position only (see @p EtaExp).
///
/// The phase flattens **one level** of a lambda's (thresholded) parameters per run.
/// Because it is scheduled inside a fixed-point pipeline (`%compile.phases tt (...)`),
/// re-running it converges to a full flatten; each run that peels calls invalidate().
///
/// Flattening respects Flags::scalarize_threshold via the thresholded projection helpers
/// (Def::num_tprojs, Lam::tvar, App::targ): a parameter is only expanded if its arity is
/// below the threshold.
/// It will not flatten mutable @p Sigma%s or @p Arr%ays (their vars have no static arity).
///
/// A parameter that is @p Extract%ed / @p Insert%ed via a **non-constant** index is left
/// intact: splitting it would only force the body to reassemble the tuple.
class Scalarize : public RWPhase {
private:
    /// Optimistic fixed-point analysis: every optimizable @p Lam is assumed splittable
    /// until proven otherwise (escape, branch/dispatch tuple disagreement, or a
    /// dynamically-indexed parameter).
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "Scalarize::Analysis") {}

        /// Per-parameter expand mask for @p lam (length @p lam->num_tvars()); `true` marks
        /// a parameter to be flattened one level. An **empty** mask means "leave untouched".
        /// Cheap; recomputed on demand from the (post-fixed-point) lattice.
        Vector<bool> plan(Lam* lam);

    private:
        const Def* rewrite(const Def* old) final;

        void inspect(const Def* def);
        /// Marks parameter @p dom of @p lam as *keep whole* by OR-ing bit @p dom into a per-lam bitmask stored
        /// in lattice() under the (stable) `lam->var()` key.
        /// We store the fact directly - **not** via lattice(concr, abstr)/pin_top() - because those also seed the
        /// rewriter map(), which this same-World Analysis would then rebuild dependent types under (and never
        /// converge). The key must be stable: `lam->tvar(dom)` is *not* (a dependent projection re-mints on every
        /// call), whereas `lam->var()` is; hence the bitmask-on-var encoding. A fresh bit invalidate()s.
        void keep(Lam* lam, size_t dom);
        void demote(Lam* lam); ///< Marks the whole @p lam *keep whole* via a ⊤ sentinel (`var ↦ var`).
        void demote_all(const Def* lam_tuple);
        bool kept(Lam* lam, size_t dom) const; ///< Is parameter @p dom of @p lam kept whole?
        bool untouched(Lam* lam) const;        ///< No fact recorded for @p lam at all.
        bool eligible(Lam*) const;             ///< Cn with an immutable (non-dependent) Pi.
    };

public:
    Scalarize(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;

    /// Flattens @p app%'s arguments one level according to @p mask.
    DefVec flatten_args(const App* app, const Vector<bool>& mask);

    Analysis analysis_;
};

} // namespace mim
