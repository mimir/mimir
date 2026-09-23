#pragma once

#include "mim/phase.h"

namespace mim {

/// Aggressive Dead Code Elimination.
class ADCE : public RWPhase {
public:
    using Super = RWPhase;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;
        using Mask  = Vector<bool>;

        Analysis(World& world)
            : mim::Analysis(world, "ADCE::Analyzer") {}

        void reset() final;

    private:
        const Proxy* mk_proxy(const Def* var);
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

    ADCE(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const App*, Lam*);

    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// An empty Mask means: leave @p lam alone.
    Mask statics(Lam* lam);

    DefSet analyzed_;
    LamMap<Vector<Mask>> lam2sites_;
};

} // namespace mim
