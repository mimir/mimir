#pragma once

#include "mim/def.h"
#include "mim/phase.h"
#include "mim/rewrite.h"
#include "mim/schedule.h"

namespace mim::plug::cps {

/// Rewrite cps.cps2ds to cps.
class CPS2DSPhase : public Phase {
public:
    CPS2DSPhase(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() final;

private:
    const Def* rewrite(const Def* def);
    const Def* rewrite_lam(Lam* lam);

    Lam* make_continuation(const Def* cn_type, const Def* arg, Sym prefix);
    Lam* result_lam(Lam* lam);
    Scheduler& scheduler(const Def*);
    const Nest& curr_external_nest() const;

    Lam2Lam lam2lam_;
    Def2Def rewritten_;
    DefMap<std::unique_ptr<Nest>> nests_;
    DefMap<Scheduler> scheduler_;
    Lam* current_external_;
};

} // namespace mim::plug::cps
