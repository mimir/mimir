#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::mem::phase {

/// The general idea of this phase is to change the shape of signatures of functions.
/// The %%mem.M is special: some binaries take exactly one memory as argument.
///
/// * `Flat`: values are extracted, mems are the first argument
///   `[[mem, [I32, I32], A], Cn [mem, I32]]` becomes `[mem, I32, I32, A, Cn [mem, I32]]`
/// * `Arg`: `[[mem, args], ret]`
///   `[mem, I32, I32, A, Cn [mem, I32]]` becomes `[[mem, [I32, I32, A]], Cn [mem, I32]]`
class Reshape : public RWPhase {
public:
    enum Mode { Flat, Arg };

    Reshape(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    void apply(Mode);
    void apply(const App* app) final;
    void apply(Phase& p) final { apply(static_cast<Reshape&>(p).mode()); }

    Mode mode() const { return mode_; }

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Reshapes a type into its flat or arg representation.
    const Def* reshape_type(const Def* T);
    /// Reshapes a def into its flat or arg representation.
    const Def* reshape(const Def* def);
    /// This generalized version of reshape transforms def to match the shape of target.
    const Def* reshape(const Def* def, const Def* target);
    const Def* reshape(DefVec& defs, const Def* T, const Def* mem);

    Mode mode_;
};

} // namespace mim::plug::mem::phase
