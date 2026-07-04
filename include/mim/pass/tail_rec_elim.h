#pragma once

#include "mim/pass.h"

namespace mim {

class EtaRed;

class TailRecElim : public FPPass<TailRecElim, Lam> {
public:
    TailRecElim(World& world, flags_t annex)
        : FPPass(world, annex) {}

    void init(PassMan*) final;

private:
    /// @name PassMan hooks
    ///@{
    const Def* rewrite(const Def*) override;
    undo_t analyze(const Def*) override;
    ///@}

    LamMap<std::pair<Lam*, Lam*>> old2rec_loop_;
    Lam2Lam rec2loop_;
};

} // namespace mim
