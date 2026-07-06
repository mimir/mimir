#pragma once

#include "mim/phase.h"

#include "mim/util/util.h"

#include "fe/assert.h"

namespace mim {

/// This phase takes care that Lam%das appear either **only** in callee position (Known) or not (Unknown).
/// If a function `f` is both Known and Unknown,
/// this Phase will η-expand the Unknown occurance which makes the function Known: `g f -> g (λx.f x)`
class EtaExp : public RWPhase {
public:
    EtaExp(World& world)
        : RWPhase(world, "EtaExp") {}
    EtaExp(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    enum Lattice : u8 {
        None      = 0,
        Known     = 1,
        Unknown_1 = 2,
        Unknown_N = 3,
        Both      = 4,
    };

    static Lattice join(Lattice l1, Lattice l2) {
        if (l1 == Unknown_1 && l2 == Unknown_1) return Unknown_N;
        if (l1 == l2) return l1;
        if (l1 == None) return l2;
        if (l2 == None) return l1;
        if (l1 == Both || l2 == Both) return Both;
        if (l1 == Known && (l2 == Unknown_1 || l2 == Unknown_N)) return Both;
        if (l2 == Known && (l1 == Unknown_1 || l1 == Unknown_N)) return Both;
        if (l1 == Unknown_1 && l2 == Unknown_N) return Unknown_N;
        if (l2 == Unknown_1 && l1 == Unknown_N) return Unknown_N;
        fe::unreachable();
    }

    static bool eta_expand(Lattice l) { return l != Known && l != Unknown_1; }
    bool eta_expand(const Lam* lam) { return eta_expand(lattice(lam)); }

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

    void rewrite_annex(flags_t, Sym, const Def*) final;
    void rewrite_external(Def*) final;
    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Var(const Var*) final;
    const Def* rewrite_no_eta(const Def* old_def) { return RWPhase::rewrite(old_def); }

    DefSet analyzed_;
    GIDMap<const Lam*, Lattice> lam2lattice_;
};

} // namespace mim
