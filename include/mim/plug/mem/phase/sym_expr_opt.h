#pragma once

#include "mim/def.h"
#include "mim/phase.h"

#include "fe/assert.h"

namespace mim::plug::mem::phase {

/// Symbolic Expression Optimization. Combines:
/// * *[Constant propagation with conditional branches](https://dl.acm.org/doi/pdf/10.1145/103135.103136)* but
/// propagates arbitrary expressions
/// * *[Detecting equality of variables in programs](https://dl.acm.org/doi/10.1145/73560.73561)*
/// * Much in the spirit of *[Combining analyses, combining
/// optimizations](https://dl.acm.org/doi/pdf/10.1145/201059.201061)*.
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
class SymExprOpt : public RWPhase {
private:
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "SEO::Analyzer") {}

        void run() final {
            mim::Analysis::run();
            DLOG("lattice:");
            for (auto [k, v] : lattice_)
                DLOG("{} -> {}", k, v);

            DLOG("done running");
        }

        void start() final;
        void reset() final;

        const Def* slot2value(const Def* slot);

        const Def2Def& all_slots() const { return all_slots_; }
        const DefMap<Def2Def>& mut2slot2value() const { return mut2slot2value_; }

    private:
        const Def* slot2value(const Def* slot, const Def* value) { return mut2slot2value_[curr_mut()][slot] = value; }
        const Def* propagate(const Def*, const Def*);
        DefVec gvn(DefVec &, DefVec &);
        const Def* rewrite_imm_App(const App*) final;

        // post-processing analysis to find sloxies that must be set to top
        void analyze(const Def*);

        DefMap<Def2Def> mut2slot2value_;
        Def2Def sloxy2slot_;
        Def2Def all_slots_;
        DefSet visited_;
    };

public:
    SymExprOpt(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    Analysis analysis_;
    Lam2Lam lam2lam_;
};

} // namespace mim::plug::mem::phase
