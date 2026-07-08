#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::mem::phase {

/// Symbolic Expression Optimization.
/// Based on [SSA Translation Is an Abstract Interpretation](https://dl.acm.org/doi/10.1145/3571258).
/// In addition:
/// * propagates whole expressions - not just constants
/// * optimistically combines φs/Vars that are already present with those being constructed through SSA translation
/// * since abstract domain is a MimIR expression, stack slots themselves can be propagated etc.
///
/// Additional papers worth reading:
/// * [Constant propagation with conditional branches](https://dl.acm.org/doi/pdf/10.1145/103135.103136)
/// * [Detecting equality of variables in programs](https://dl.acm.org/doi/10.1145/73560.73561)
/// * [Combining analyses, combining optimizations](https://dl.acm.org/doi/pdf/10.1145/201059.201061)
/// * [Simple and efficient construction of static single assignment
/// form](https://dl.acm.org/doi/10.1007/978-3-642-37051-9_6)
///
/// Due to MimIR's sea of node structure a number of other optimizations kick in such as arithmetic simplifications and
/// code motion.
///
/// Lattice per Lam::var:
/// ```
///  ⊤      ← Keep as is
///  |
/// Bundle  ← Vars that (horizontally) behave the same build a single congruence class
///  |
/// Expr    ← Whole expression is propagated (vertically) through var
///  |
///  ⊥
/// ```
class SEO : public RWPhase {
private:
    using Super = mim::RWPhase;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;

        Analysis(World& world)
            : mim::Analysis(world, "SEO::Analyzer") {}

        void reset() final;

        const DefSet& slots() const { return slots_; }
        const DefMap<Def2Def>& mut2sloxy2val() const { return mut2sloxy2val_; }

    private:
        const Def* sloxy2val(const Def* sloxy);
        const Def* sloxy2val(const Def* sloxy, const Def* val) { return mut2sloxy2val_[curr_mut()][sloxy] = val; }

        const Def* sccp_join(const Def*, const Def*);
        DefVec sccp(Defs, Defs);
        void gvn_bundle(Defs, Defs, Span<const Def*>);
        void gvn_split(Defs, Span<const Def*>, Span<const Def*>);
        DefVec sccp_gvn(Defs, Span<const Def*>);

        const Def* rewrite_imm_App(const App*) final;

        // post-processing analysis to find sloxies that must be set to top
        void finalize() final;
        void analyze(const Def*);

        // local (reset between iterations)
        DefMap<Def2Def> mut2sloxy2val_;
        DefSet visited_;

        // global (kept between iterations)
        Def2Def sloxy2slot_;
        DefSet slots_; // actually slot ptrs
    };

public:
    SEO(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    /// Does @p old_lam have propagated vars or live phis and hence needs a new signature?
    bool needs_seo(Lam* old_lam);
    /// Builds (and caches) the new Lam for @p old_lam with propagated vars removed and kept phis appended.
    Lam* build_lam(Lam* old_lam);
    /// Builds the argument list for an App of @p old_lam matching the signature built by build_lam().
    DefVec build_args(Lam* old_lam, const App* old_app);
    /// Rewrites the value of @p sloxy as known at the current call site:
    /// either the value curr_mut() wrote to the slot or curr_mut()'s own phi for it.
    const Def* rewrite_site_value(const Def* sloxy, const Def* slot_type);

    Analysis analysis_;
    Lam2Lam lam2lam_;
};

} // namespace mim::plug::mem::phase
