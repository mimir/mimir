#pragma once

#include <optional>

#include <mim/phase.h>

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

/// Operator Strength Reduction.
/// Replaces each `core.wrap.mul (i, c)` / `core.nat.mul (i, c)` by a new param `j` of Lam `head`,
/// where `i` is an induction variable of `head` and `c` is invariant in `head`.
/// Wherever `head` is entered with `i ↦ i ± t`, `j` advances by `t * c`; any other call site passes `x * c` for `i ↦
/// x`. Hence, the rewrite stays correct at every call site; the analysis only decides whether it pays off. This
/// includes the saturating `core.nat.sub`, as `(a ∸ b) * c = a * c ∸ b * c`.
/// @see [Operator Strength Reduction](https://dl.acm.org/doi/10.1145/504709.504710)
///
/// Lattice per Lam::var:
/// ```
///  ⊤      ← Keep as is
///  |
///  IV     ← Proxy: every call site passes the var itself, the var ± an invariant step, or a value from outside
///  |
/// Expr    ← Whole expression is propagated through the var, so a step may pass through intermediate Lam%s
///  |
///  ⊥
/// ```
/// Joining two different values optimistically assumes IV.
/// The next round then sees the var as Proxy and checks each step.
class OSR : public RWPhase {
public:
    /// The subtag core.nat and core.wrap share.
    enum class Op : sub_t { add, sub, mul };

    /// `i ↦ i op t`; `t == nullptr` means `i ↦ i`.
    struct Step {
        Op op;
        const Def* t;
    };

    struct Cand {
        size_t i; ///< Index of the IV in its Lam's tvars.
        const Def* c;

        bool operator==(const Cand&) const = default;
    };

    using Steps = fe::Vector<std::optional<Step>>;
    using Cands = fe::Vector<Cand>;

private:
    using Super = RWPhase;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;

        Analysis(World& world)
            : mim::Analysis(world, "OSR::Analysis") {}

        void reset() final;

        bool is_iv(const Def* var) const;
        const Steps* steps(const App* app) const { return fe::lookup(app2steps_, app); }
        const Cands* cands(Lam* lam) const { return fe::lookup(lam2cands_, lam); }

    private:
        const Def* join(const App* site, Lam* lam, size_t i, const Def* abstr_arg);
        void cand(const Def* x, const Def* c);
        const Def* rewrite_imm_App(const App*) final;

        void finalize() final;
        void escape(const Def*);

        DefSet visited_;
        DefSet first_;
        DefSet stepped_;
        DefMap<Steps> app2steps_;
        LamMap<Cands> lam2cands_;
        LamMap<MutSet> lam2callers_;
    };

public:
    OSR(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// The Cand%s of @p lam that become a new param, in that order.
    const Cands& cands(Lam* lam);
    Lam* build_lam(Lam* old_lam);
    const Def* reduced(const Def* x, const Def* c);

    Analysis analysis_;
    LamMap<Cands> lam2cands_;
    LamBiMap lam_old2new_;
};

} // namespace mim::plug::core::phase
