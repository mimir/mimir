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
///
/// The transformation is **type-directed**:
/// the decision to flatten is made per continuation *type* (immutable `Cn`) - not per Lam.
/// Every producer and consumer of such a type is reshaped in the same sweep:
/// the Pi itself, all Lam%s of that type, and all App%s through it - no matter whether the callee is a mutable Lam,
/// a Var (higher-order parameter), a branch tuple, or a value loaded back from memory.
/// Hence, a Lam may escape (be stored, passed as argument) and still get its signature flattened.
///
/// A Pi is *pinned* (left untouched) if
/// * it is reachable from an annex (normalizers and backends rely on its exact shape),
/// * an Axm application's signature dictates it - what the Axm consumes and produces,
///   plus a *bare* function argument's type (`%%autodiff.ad f`) - except for subtrees that are
///   merely substituted in via (type) arguments (`T` in `%%mem.store T`), which stay flattenable,
/// * it occurs inside an *interface* Lam's signature (external, annex, or unset declaration) -
///   only such a Lam's own top-level Pi stays flattenable (it may be shared with internal values;
///   rewrite_mut_Lam() preserves the interface's top level by hand),
/// * it types a value inside a dependently-typed aggregate (a typed closure),
/// * an App connects a dom and an arg whose types are alpha-equivalent yet *distinct* defs, or
/// * one of its parameters is Extract%ed / Insert%ed via a **non-constant** index
///   (splitting would only force the body to reassemble the tuple);
///   this is tracked per parameter via a keep-bitmask.
///
/// The phase flattens **one level** of a Pi's (thresholded) domain per run.
/// Because it is scheduled inside a fixed-point pipeline (`%compile.phases tt (...)`),
/// re-running it converges to a full flatten; each run that peels calls invalidate().
///
/// Flattening respects Flags::scalarize_threshold via the thresholded projection helpers
/// (Def::num_tprojs, Pi::tdom, App::targ): a parameter is only expanded if its arity is
/// below the threshold.
/// It will not flatten mutable @p Sigma%s or @p Arr%ays (their vars have no static arity).
class Scalarize : public RWPhase {
private:
    /// Optimistic fixed-point analysis: every immutable `Cn` is assumed flattenable
    /// until proven otherwise (see the pinning rules above).
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "Scalarize::Analysis") {}

        /// Per-parameter expand mask for @p type (length `num_tdoms`); `true` marks
        /// a parameter to be flattened one level. An **empty** mask means "leave untouched".
        /// Cheap; recomputed on demand from the (post-fixed-point) lattice.
        Vector<bool> plan(const Def* type) const;

    private:
        const Def* rewrite(const Def* old) final;

        void inspect(const Def* def);
        /// Marks parameter @p dom of @p pi as *keep whole* by OR-ing bit @p dom into a per-Pi bitmask stored
        /// in lattice() under @p pi.
        /// We store the fact via lattice_force() - **not** via lattice(concr, abstr)/pin():
        /// growing the mask swaps one Nat literal for another - non-monotone in terms of Def%s.
        /// As with any lattice write, the bitmask also lands in the rewriter map(),
        /// short-circuiting later rewrite(pi) calls to the Nat.
        /// That is harmless for this same-World Analysis:
        /// drain() discards rewrite results, and inspect() always receives the old def.
        /// A fresh bit invalidate()s.
        void keep(const Pi* pi, size_t dom);
        void pin_tree(const Def* def); ///< pin%s every flattenable Pi nested in @p def%'s type tree.
        /// As above, but skips defs already in @p visited - seed it to exempt subtrees from pinning.
        void pin_tree(const Def* def, DefSet& visited);
        bool kept(const Pi* pi, size_t dom) const; ///< Is parameter @p dom of @p pi kept whole?
    };

public:
    Scalarize(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_Pi(const Pi*) final;
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;

    /// Flattens @p app%'s arguments one level according to @p mask.
    DefVec flatten_args(const App* app, const Vector<bool>& mask);

    Analysis analysis_;
};

} // namespace mim
