#pragma once

#include "mim/phase.h"

#include "mim/util/util.h"

namespace mim {

/// This phase takes care that Lam%das appear either **only** in callee position (Known) or not (Unknown).
/// If a function `f` is both Known and Unknown,
/// this Phase will η-expand the Unknown occurance which makes the function Known: `g f -> g (λx.f x)`
class EtaExpPhase : public RWPhase {
public:
    EtaExpPhase(World& world)
        : RWPhase(world, "EtaExpPhase") {}
    EtaExpPhase(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    enum Lattice : u8 {
        None    = 0,
        Known   = 1 << 0,
        Unknown = 1 << 1,
        Both    = Known | Unknown,
    };

    static Lattice join(Lattice l1, Lattice l2) { return Lattice((u8)l1 | (u8)l2); }

    void join(const Lam* lam, Lattice l) {
        if (auto [i, ins] = lam2lattice_.emplace(lam, l); !ins) i->second = join(i->second, l);
    }

    Lattice lattice(const Lam* lam) {
        if (auto i = lam2lattice_.find(lam); i != lam2lattice_.end()) return i->second;
        return None;
    }

    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, Lattice);

    void rewrite_annex(flags_t, const Def*) final;
    void rewrite_external(Def*) final;
    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Var(const Var*) final;
    const Def* rewrite_no_eta(const Def* old_def) { return Rewriter::rewrite(old_def); }

    DefSet analyzed_;
    GIDMap<const Lam*, Lattice> lam2lattice_;
    GIDMap<const Lam*, const Def*> lam2eta_;
};

} // namespace mim
