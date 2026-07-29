#pragma once

#include "mim/phase.h"

namespace mim {

/// Eliminates tail recursion:
/// A returning Lam `f` that calls itself with its own ret var as continuation is split into
/// a wrapper `rec` (same signature) and a `loop` basic block (signature without the ret var);
/// the recursive call becomes a jump to `loop`.
class TailRecElim : public RWPhase {
public:
    TailRecElim(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Does @p lam call itself in tail position (with its own ret var as continuation)?
    bool is_tail_rec(Lam* lam);

    LamMap<bool> tail_rec_;
    LamMap<std::pair<Lam*, Lam*>> old2rec_loop_;
};

} // namespace mim
