#pragma once

#include <optional>

#include <mim/phase.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem {

/// Determines which allocations stay within the scope that allocates them.
///
/// The *home* of an allocation is the Lam on whose memory chain it sits.
/// An allocation *escapes* if it flows into the data of a tail call - returned, or handed to a callee or continuation -
/// or if an escaping Lam mentions it.
/// Any op but `mem.load`, the address of a `mem.store`, and a `mem.free` of the allocation itself may retain the
/// pointers it is handed, so they escape there. A Lam escapes if its use is *unknown* - it is used as a value - or if
/// an escaping Lam jumps to it. An axiom in control position is assumed to only pass control on to the continuations it
/// receives.
///
/// A buffer-typed parameter of a Lam takes on the value its known call sites pass, as in SCCP.
/// An external Lam has unknown call sites, so whatever is passed to it escapes.
/// A value only flows into a Lam that its scope strictly encloses; otherwise it would outlive its scope and escapes.
///
/// Lattice: an escaping allocation is ⊤ - keep as is; *absent* means it stays local.
/// A parameter is ⊤ if its call sites disagree; everything they pass escapes.
class EscapeAnalysis : public mim::Analysis {
public:
    struct Chain {
        const App* tail;
        fe::Vector<const App*> ops; ///< The ops threading the memory from the Lam's Var to its tail, in program order.
        DefMap<size_t> last;        ///< Per allocation the ops mention: the index of the last op that does so.
        DefVec targets;             ///< The continuations the tail transfers control to.
    };

    EscapeAnalysis(World& world)
        : mim::Analysis(world, "mem::EscapeAnalysis") {}

    static bool allocates(const App* app) { return Axm::isa<mem::alloc>(app) || Axm::isa<mem::malloc>(app); }

    /// @name Results
    ///@{
    /// The Lam%s with an intact memory chain.
    const LamMap<Chain>& chains() const { return chains_; }
    /// May @p lam run at any time, as it is used as a value or jumped to by an escaping Lam?
    bool escaping(Lam* lam) const { return escaping_.contains(lam); }
    /// Is the local @p alloc only mentioned on its home's own memory chain?
    bool confined(const Def* alloc) const { return !shared_.contains(alloc); }
    /// Does a `mem.free` release @p alloc?
    bool freed(const Def* alloc) const { return freed_.contains(alloc); }
    /// The allocation @p def releases if it is a `mem.free` of one; `nullptr` otherwise.
    static const App* freed_alloc(const Def* def) {
        if (auto free = Axm::isa<mem::free>(def))
            if (auto ex = free->arg(2, 1)->isa<Extract>())
                if (auto app = ex->tuple()->isa<App>(); app && allocates(app)) return app;
        return nullptr;
    }
    ///@}

private:
    /// Records what each value may carry: the allocations it aliases and the nested Lam%s in it.
    const Def* rewrite_imm(const Def*) override;
    const Def* rewrite_imm_App(const App*) override;
    void leave() override;

    bool may_hold(const Def* type) const;
    std::optional<fe::Vector<const App*>> walk(Lam* lam, const App* tail);
    /// Yields where @p mem starts in @p lam - its memory Var or the memory it captures - or `nullptr` if lost.
    /// Collects the @p ops on the way in reverse program order.
    const Def* walk(Lam* lam, const Def* mem, fe::Vector<const App*>& ops);
    /// The memory @p tail hands on - directly or captured by the continuation of a direct-style call.
    const Def* exit_mem(const App* tail, LamSet& done);
    /// Adds what @p def may carry to @p carried, possibly twice; a nested Lam carries itself.
    void join(DefVec& carried, const Def* def) const;

    void escape(Lam* lam) {
        if (escaping_.emplace(lam).second) taint(lam), touch();
    }
    /// Everything @p def may carry escapes.
    void escape(const Def* def);

    /// The Lam%s a tail jumps to directly - itself or through a jump table - or nothing.
    static fe::Vector<Lam*> jumps(const Def* callee);
    void jump(Lam* k, const App* tail, DefVec& targets);
    /// Joins @p val into @p k%'s parameter @p var.
    void propagate(Lam* k, const Def* var, const Def* val);
    /// A continuation handed to a tail transfers control; anything else escapes.
    void pass(const Def* def, DefVec& targets) { Pi::isa_cn(def->type()) ? control(def, targets) : escape(def); }
    /// Collects the continuations a tail transfers control to; the data along the way escapes.
    void control(const Def* def, DefVec& targets);

    LamMap<Chain> chains_;
    DefMap<DefVec> aliases_; ///< What a value may carry; *absent* means nothing.
    DefSet shared_;
    DefSet freed_;
    LamSet escaping_;
};

} // namespace mim::plug::mem
