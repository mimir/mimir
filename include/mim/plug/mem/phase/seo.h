#pragma once

#include <absl/container/btree_set.h>

#include <mim/def.h>
#include <mim/phase.h>

#include <mim/util/util.h>

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
/// A var that has reached ⊤ for propagation but still awaits GVN bundling is marked with a dedicated Proxy sentinel.
class SEO : public RWPhase {
private:
    using Super = mim::RWPhase;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;

        Analysis(World& world)
            : mim::Analysis(world, "SEO::Analyzer") {}

        void reset() final;

        const LamSet& unknowns() const { return unknowns_; }

        // SSA
        const auto& slots() const { return slots_; }
        const auto& lam2sloxy2val() const { return lam2sloxy2val_; }
        const Def* lam2sloxy2val(Lam* lam, const Def* sloxy);

    private:
        // SCCP
        const Proxy* mk_sccp_top(const Def* var);
        const Def* sccp_join(Lam*, const Def*, const Def*);
        DefVec sccp(Lam*, Defs vars, Defs abstr_args);

        /// Applies @p known to @p abstr_targs (one per tvar): propagates phis, runs SCCP + GVN, and sets the vars.
        const Def* apply_known(Lam* known, Defs abstr_targs);

        // GVN
        const Proxy* mk_bundle(Lam* lam, const Def* var, Defs bundle_vars);
        void gvn_bundle(Lam*, Defs, Defs, Span<const Def*>);
        void gvn_split(Lam*, Defs, Span<const Def*>, Span<const Def*>);

        // SSA
        void propagate_phis(Lam*, DefVec& vars, DefVec& abstr_args);
        const Def* sloxy2val(const Def* sloxy) { return lam2sloxy2val(curr_mut<Lam>(), sloxy); }
        const Def* sloxy2val(const Def* sloxy, const Def* val) { return lam2sloxy2val_[curr_mut<Lam>()][sloxy] = val; }

        const Def* rewrite_imm_App(const App*) final;

        // post-processing analysis to find sloxies that must be set to top
        void finalize() final;
        void analyze(const Def*);

        // local (reset between iterations)
        absl::node_hash_map<Lam*, Def2Def, GIDHash<const Def*>> lam2sloxy2val_;
        DefSet visited_;
        DefSet first_;

        // Scratch for find_unknowns; cleared per query instead of constructing containers per App.
        // fu_lams_ is a Vector, not a LamSet: find_unknowns pushes each Lam at most once (visited_ gates
        // before the Lam check), and insertion order follows the structural deps() walk - so it is
        // deterministic without a sort, and clear() always keeps its capacity.
        DefSet fu_visited_;
        Vector<Lam*> fu_lams_;

        // global (kept between iterations)
        Def2Def sloxy2slot_;
        absl::btree_set<const Def*, GIDLt<const Def*>> slots_; // actually slot ptrs
        LamSet unknowns_;            // Lam%s reached as a *value*; their signature must stay untouched
        LamMap<MutSet> lam2callers_; // all muts that apply a Lam; tainted when the Lam's abstract vars change
    };

public:
    SEO(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// A live phi for @p old_lam: the @p sloxy it stands for, the @p phi proxy, and its abstract @p val.
    struct Phi {
        const Def* sloxy;
        const Def* phi;
        const Def* val;
    };

    /// Was the SSA construction able to eliminate this sloxy?
    const Def* isa_optimized_sloxy(const Def*) const;
    /// The (memoized) live phis of @p old_lam.
    const Vector<Phi>& phis_of(Lam* old_lam);
    /// Does @p old_lam have propagated vars or live phis and hence needs a new signature?
    bool needs_seo(View<Phi>, Lam* old_lam);
    /// Builds (and caches) the new Lam for @p old_lam with propagated vars removed and kept phis appended.
    Lam* build_lam(View<Phi>, Lam* old_lam);
    /// Builds the argument list for a jump to @p old_lam (with the given @p old_targs, one per tvar)
    /// matching the signature built by build_lam().
    DefVec build_args(View<Phi>, Lam* old_lam, Defs old_targs);

    Analysis analysis_;
    Lam2Lam lam_old2new_;
    Lam2Lam lam_new2old_;
    absl::node_hash_map<Lam*, Vector<Phi>, GIDHash<Lam*>> lam2phis_;
};

} // namespace mim::plug::mem::phase
