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
    using Super   = mim::RWPhase;
    using Var2Idx = DefMap<size_t>;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;

        Analysis(World& world)
            : mim::Analysis(world, "SEO::Analyzer") {}

        void reset() final;

        const Def* slot2value(const Def* slot);

        const Def2Def& all_slots() const { return slot2type_; }
        const DefMap<Def2Def>& mut2slot2value() const { return mut2slot2value_; }

    private:
        const Def* slot2value(const Def* slot, const Def* value) { return mut2slot2value_[curr_mut()][slot] = value; }
        const Def* sccp_join(const Def*, const Def*);
        DefVec sccp(Defs, Defs);
        void gvn_bundle(Defs, Defs, Span<const Def*>, const Var2Idx&);
        void gvn_split(Defs, Span<const Def*>, Span<const Def*>, const Var2Idx&);
        DefVec sccp_gvn_propagate(Defs, Span<const Def*>);
        const Def* rewrite_imm_App(const App*) final;
        /// Traverses each mut only once per fixed-point round; otherwise cyclic CFGs recurse forever.
        Def* rewrite_deps(Def*) final;

        // post-processing analysis to find sloxies that must be set to top
        void finalize() final;
        void analyze(const Def*);

        DefMap<Def2Def> mut2slot2value_;
        Def2Def sloxy2slot_;
        Def2Def slot2type_;
        DefSet visited_;
        MutSet deps_done_;
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
