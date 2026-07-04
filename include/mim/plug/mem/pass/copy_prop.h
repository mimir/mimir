#pragma once

#include <mim/pass.h>

namespace mim {

namespace plug::mem::pass {

/// This FPPass is similar to sparse conditional constant propagation (SCCP).
/// However, this optmization also works on all Lam%s alike and does not only consider basic blocks as opposed to
/// traditional SCCP. What is more, this optimization will also propagate arbitrary Def%s and not only constants.
/// Finally, it will also remove dead Var%s.
class CopyProp : public FPPass<CopyProp, Lam> {
public:
    CopyProp(World& world, flags_t annex)
        : FPPass(world, annex) {}

    void apply(bool bb_only);
    void apply(const App* app) final { apply(Lit::as<bool>(app->arg())); }
    void apply(Stage& s) final { apply(static_cast<CopyProp&>(s).bb_only()); }
    void init(PassMan*) final;

    bool bb_only() const { return bb_only_; }

    using Data = LamMap<DefVec>;

private:
    /// Lattice used for this Pass:
    /// ```
    ///  Keep    <-- We cannot do anything here.
    ///   |
    ///  Prop    <-- Var is live but the same Def everywhere.
    ///   |
    ///  Dead    <-- Var is dead.
    /// ```
    enum Lattice : u8 { Dead, Prop, Keep };
    enum : u32 { Varxy, Appxy };
    using Lattices = Vector<Lattice>;

    /// @name PassMan hooks
    ///@{
    const Def* rewrite(const Def*) override;
    undo_t analyze(const Proxy*) override;
    ///@}

    LamMap<std::tuple<Lattices, Lam*, DefVec>> lam2info_;
    bool bb_only_;
};

} // namespace plug::mem::pass
} // namespace mim
