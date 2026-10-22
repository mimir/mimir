#pragma once

#include "mim/phase.h"

namespace mim {

/// Eliminates all types that have exactly one inhabitant, so that no Single/Wrap ever reaches a backend.
/// Such a type carries no information, so its erasure is the unit: `«e»` becomes `[]` and `‹e›` becomes `()`.
/// Within an aggregate the component is dropped altogether, which shifts the indices of its successors.
/// Besides Single this also catches the unit `[]` and `Idx 1` - and any aggregate built from those.
/// A Variant with a single case `| A` is isomorphic to `A` and erased to it.
/// A singleton above `*` has no unit to erase to and degrades to the type of its inhabitant instead.
/// Where an erased value meets a shape that still demands its components (e.g. an Axm's signature), it is coerce()d:
/// dropped components are re-inserted as their sole inhabitants, and Cn%s are η-wrapped.
class SingleErasure : public RWPhase {
public:
    SingleErasure(World& world, std::string name)
        : RWPhase(world, std::move(name)) {}
    SingleErasure(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// @name Erasure
    /// All arguments live in old_world(), all results in new_world().
    ///@{
    bool is_unit(const Single* single) { return single->type() == old_world().type<0>(); }
    const Def* erase(const Single*);        ///< The erasure of the singleton *type* itself.
    const Def* inhabitant(const Def* type); ///< The sole inhabitant of the information-free @p type.
    Sieve sieve(const Sigma*);              ///< The components of @p sigma that survive.
    /// The literal @p index into a Sigma, adjusted for the components @p keep dropped.
    size_t remap(const Sieve& keep, const Def* index);
    ///@}

    const Def* rewrite_imm_Single(const Single*) final;
    const Def* rewrite_imm_Wrap(const Wrap*) final;
    const Def* rewrite_imm_Variant(const Variant*) final;
    const Def* rewrite_imm_Inj(const Inj*) final;
    const Def* rewrite_imm_Match(const Match*) final;
    const Def* rewrite_imm_Sigma(const Sigma*) final;
    const Def* rewrite_mut_Sigma(Sigma*) final;
    const Def* rewrite_imm_Tuple(const Tuple*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_imm_Insert(const Insert*) final;
    const Def* rewrite_imm_Seq(const Seq*) final;
    const Def* rewrite_mut_Seq(Seq*) final;
};

} // namespace mim
