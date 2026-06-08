#pragma once

#include "mim/phase.h"

namespace mim {

class RetWrap : public RWPhase {
public:
    RetWrap(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    enum Lattice : u8 { Bot, Single, Eta };

    static Lattice join(Lattice l1, Lattice l2) {
        if (l1 == Bot) return l2;
        if (l2 == Bot) return l1;
        return Eta;
    }

    void join(const Def* def, Lattice l) {
        if (auto [i, ins] = def2lattice_.emplace(def, l); !ins) i->second = join(i->second, l);
    }

    Lattice lattice(const Def* def) {
        if (auto i = def2lattice_.find(def); i != def2lattice_.end()) return i->second;
        return Bot;
    }

    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, Lattice = Eta);

    const Def* rewrite(const Def*) final;
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_no_eta(const Def* old_def) { return RWPhase::rewrite(old_def); }

    DefSet analyzed_;
    DefMap<Lattice> def2lattice_;
    DefMap<const Def*> def2eta_;
    VarMap<const Def*> var2def_; // vars that contain a ret_var
    LamSet split_;
};

} // namespace mim
