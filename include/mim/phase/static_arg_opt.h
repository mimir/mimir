#pragma once

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
    /// @p aggr clones the recursion so that *every* param static at *some* call site becomes static in *some*
    /// specialization; off by default, as it trades code size for that.
    StaticArgOpt(World& world, flags_t annex, bool aggr = false)
        : RWPhase(world, annex)
        , aggr_(aggr) {}

    void apply(bool aggr);
    void apply(const App*) final;
    void apply(Phase&) final;

private:
    /// One bit per Lam::tdom; thresholded, so a huge sigma dom stays a single bit.
    using Mask = Vector<bool>;

    /// A specialization: `wrap` keeps the full signature, `loop` drops the doms marked in Clone::d.
    struct Clone {
        Mask d;
        Lam* wrap;
        Lam* loop;
    };

    bool analyze() final;
    void analyze(const Def*);
    void visit(const App*, Lam*);
    void finalize() final;

    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Which of @p lam's doms do the self-calls selected for the loop forward unchanged?
    /// An empty Mask means: leave @p lam alone.
    Mask statics(Lam* lam);
    /// A Lam nested inside another Lam has to be a basic block, so the loop must shed the ret var.
    static bool sheds_ret(Lam* lam, const Mask& d) { return !lam->ret_pi() || (!d.empty() && d.back()); }
    /// Finds or creates the specialization of @p lam for @p d; the `bool` tells whether it was created.
    std::pair<Clone, bool> clone(Lam* lam, const Mask& d);
    void specialize(Lam* lam, const Clone&, bool cloned);

    bool aggr_;
    /// Nest of the Lam being cloned; everything depending on it must be rewritten afresh per specialization.
    std::unique_ptr<const Nest> nest_;
    DefSet analyzed_;
    LamMap<Vector<Mask>> lam2sites_;
    LamMap<Mask> lam2statics_;
    LamMap<Vector<Clone>> lam2clones_;
    std::deque<std::pair<Lam*, Mask>> todo_clones_;
};

} // namespace mim
