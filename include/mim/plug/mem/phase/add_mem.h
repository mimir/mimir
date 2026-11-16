#pragma once

#include <mim/phase.h>
#include <mim/schedule.h>

namespace mim::plug::mem::phase {

/// Threads `mem.M 0` through the World.
/// A continuation whose domain carries no `mem.M 0` gains a leading one; hence, a second run changes nothing.
/// While rewriting a continuation's body, AddMem::curr_mem_ is the one live memory:
/// it starts as the continuation's memory parameter, each memory operation advances it, and every memory operand -
/// including a `⊥`/`⊤` placeholder - is rewired to it.
/// `mem.fresh (a, k)` jumps to `k` with it.
/// As operands are rewritten before their users, memory operations are chained in dependency order.
/// A memory operation joins the chain of the continuation Scheduler::late yields - the innermost one that nests all
/// its users - even if a nested continuation asks for it first; so all users share it.
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

    struct Frame {
        Lam* old_lam;
        const Def* mem; ///< Only up to date while the Frame is not AddMem::active_.
    };

    const Def* thread(const App*);
    void activate(size_t frame);
    Lam* thread(Lam* old_lam, Lam* new_lam);
    size_t owner(const App* app); ///: The Frame whose chain @p app joins.

    const Def* curr_mem_ = nullptr;  ///< The live memory in the new World, if any.
    fe::Vector<Frame> frames_;       ///< The continuations being rewritten.
    size_t active_        = 0;       ///< The Frame AddMem::curr_mem_ belongs to.
    Scheduler* scheduler_ = nullptr; ///< Of the function being rewritten.
    bool pinned_          = false;   ///< Are we below a Lam of AddMem::pins_?
    LamSet pins_;
};

} // namespace mim::plug::mem::phase
