#pragma once

#include "mim/phase.h"

namespace mim {

/// SCCP - Sparse Conditional Constant Propagation - but propagates **arbitrary expressions**.
/// @see [Constant propagation with conditional branches](https://dl.acm.org/doi/pdf/10.1145/103135.103136)
///
/// Lattice per Lam::var:
/// ```
///  ⊤      ← Keep as is
///  |
/// Expr    ← Whole expression is propagated (vertically) through var
///  |
///  ⊥
/// ```
/// The vars of an unknown Lam are ⊤; EtaConv beforehand confines that to a wrapper.
class SCCP : public RWPhase {
private:
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "SCCP::Analyzer") {}

    private:
        const Def* propagate(Lam* lam, const Def* var, const Def* def);
        const Def* rewrite_imm_App(const App* app) final;
        DefVec join_args(Lam*, DefVec&) final;
        void finalize() final { find_unknowns(); }
    };

public:
    SCCP(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App* old_app) final;

    Analysis analysis_;
    Lam2Lam lam2lam_;
};

} // namespace mim
