#pragma once

#include <mim/phase.h>

namespace mim::plug::mem::phase {

/// Threads `mem.M 0` through the World.
/// A continuation whose domain carries no `mem.M 0` gains a leading one; hence, a second run changes nothing.
/// While rewriting a continuation's body, AddMem::curr_mem_ is the one live memory:
/// it starts as the continuation's memory parameter, each memory operation advances it, and every memory operand -
/// including a `⊥`/`⊤` placeholder - is rewired to it.
/// `mem.fresh (a, k)` jumps to `k` with it.
/// As operands are rewritten before their users, memory operations are chained in dependency order.
///
/// Direct-style functions keep their signature.
/// So does everything reachable from an Axm's Lam arguments, as the Axm pins their type.
class AddMem : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    bool analyze() final;
    const Def* rewrite(const Def*) override;
    const Def* rewrite_mut_Lam(Lam*) override;
    const Def* rewrite_imm_App(const App*) override;
    const Def* rewrite_imm_Tuple(const Tuple*) override;
    const Def* rewrite_imm_Pi(const Pi*) override;

    const Def* curr_mem_ = nullptr; ///< The live memory in the new World, if any.
    bool pinned_         = false;   ///< Are we below a Lam of AddMem::pins_?
    LamSet pins_;
};

} // namespace mim::plug::mem::phase
