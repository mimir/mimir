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

        const auto& slots() const { return slots_; }
        const auto& mut2sloxy2val() const { return mut2sloxy2val_; }
        /// The pointee type of @p sloxy's slot, captured at slot declaration (see 507ed74: revert `pointee`).
        const Def* slot_type(const Def* sloxy) const;

    private:
        // SCCP
        const Def* sccp_join(const Def*, const Def*);
        DefVec sccp(Defs vars, Defs abstr_args);

        // GVN
        const Proxy* mk_bundle(const Def* var, Defs bundle_vars);
        void gvn_bundle(Defs, Defs, Span<const Def*>);
        void gvn_split(Defs, Span<const Def*>, Span<const Def*>);

        // SSA
        void propagate_phis(Lam*, DefVec& vars, DefVec& abstr_args);
        const Def* sloxy2val(const Def* sloxy);
        const Def* sloxy2val(const Def* sloxy, const Def* val) { return mut2sloxy2val_[curr_mut()][sloxy] = val; }

        const Def* rewrite_imm_App(const App*) final;

        // post-processing analysis to find sloxies that must be set to top
        void finalize() final;
        void analyze(const Def*);

        // local (reset between iterations)
        std::map<const Def*, Def2Def, GIDLt<const Def*>> mut2sloxy2val_;
        DefSet visited_;

        // global (kept between iterations)
        std::map<const Def*, const Def*, GIDLt<const Def*>> sloxy2slot_;
        std::map<const Def*, const Def*, GIDLt<const Def*>> sloxy2type_;
        // Slot ptrs in discovery order.
        // The order determines the phi-parameter order of rebuilt Lams, so it must be independent of gids
        // (which are not stable across runs): we keep insertion order via slots_ and dedup via slots_seen_.
        DefVec slots_;
        DefSet slots_seen_;
    };

public:
    SEO(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    /// A live phi for @p old_lam: the @p sloxy it stands for, the @p phi proxy, and its abstract @p val.
    struct Phi {
        const Def* sloxy;
        const Def* phi;
        const Def* val;
    };

    /// Does @p old_lam have propagated vars or live phis and hence needs a new signature?
    bool needs_seo(View<Phi>, Lam* old_lam);
    /// Builds (and caches) the new Lam for @p old_lam with propagated vars removed and kept phis appended.
    Lam* build_lam(View<Phi>, Lam* old_lam);
    /// Builds the argument list for an App of @p old_lam matching the signature built by build_lam().
    DefVec build_args(View<Phi>, Lam* old_lam, const App* old_app);
    /// The value curr_mut() provides for @p sloxy at this call site: the value it wrote to the slot,
    /// or - if it didn't write - curr_mut()'s own phi for it.
    const Def* site_value(const Def* sloxy);

    Analysis analysis_;
    Lam2Lam lam2lam_;
    std::map<Lam*, Vector<Phi>, GIDLt<const Def*>> lam2phis_;
};

} // namespace mim::plug::mem::phase
