#pragma once

#include <fe/bitset.h>

#include "mim/phase.h"

namespace mim {

/// Static Argument Transformation.
/// A recursive Lam whose self-calls merely forward some of its params is split into a wrapper `wrap`
/// (same signature) and a `loop` that drops those params;
/// self-calls forwarding all of them become jumps to `loop`, all others go through `wrap`.
/// Tail recursion elimination is the special case where the dropped param is the ret var.
/// @see [Compilation by Transformation in Non-Strict Functional Languages,
/// §7](https://theses.gla.ac.uk/74568/1/10992188.pdf)
class StaticArgOpt : public RWPhase {
public:
    StaticArgOpt(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const App*, Lam*);

    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Which of @p lam's doms do the self-calls selected for the loop forward unchanged?
    /// An empty Mask means: leave @p lam alone.
    fe::Bitset statics(Lam* lam);

    DefSet analyzed_;
    LamMap<fe::Vector<fe::Bitset>> lam2sites_;
    LamMap<fe::Bitset> lam2statics_;
    LamMap<std::pair<Lam*, Lam*>> old2wrap_loop_;
};

} // namespace mim
