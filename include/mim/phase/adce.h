#pragma once

#include "mim/phase.h"

namespace mim {

/// Aggressive Dead Code Elimination.
/// Removes Lam params nobody needs - including those that are merely forwarded to other dead params,
/// like a loop-carried value that never leaves its loop.
///
/// DCE is a backward problem, but an Analysis runs forward.
/// So we optimistically assume every param of a known Lam to be dead and propagate a Proxy for it.
/// Afterwards, Analysis::finalize walks the abstract World and pins each param whose Proxy survived in a live position.
/// Then, we redo.
///
/// Lattice per Lam::var:
/// ```
///  ⊤      ← Live
///  |
/// Dead    ← Proxy
///  |
///  ⊥
/// ```
class ADCE : public RWPhase {
private:
    using Super = RWPhase;

    class Analysis : public mim::Analysis {
    public:
        using Super = mim::Analysis;

        Analysis(World& world)
            : mim::Analysis(world, "ADCE::Analysis") {}

        void reset() final;
        bool is_dead(const Def* var) const;

    private:
        const Def* rewrite_imm_App(const App*) final;

        void finalize() final;
        void analyze(const Def*);

        DefSet visited_;
    };

public:
    ADCE(World& world)
        : RWPhase(world, "ADCE", &analysis_)
        , analysis_(world) {}
    ADCE(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Keeps @p lam's live tvars.
    Sieve sieve(Lam* lam);
    Lam* build_lam(Lam* old_lam);

    Analysis analysis_;
    LamMap<Sieve> lam2sieve_;
    Lam2Lam lam_old2new_;
    Lam2Lam lam_new2old_;
};

} // namespace mim
