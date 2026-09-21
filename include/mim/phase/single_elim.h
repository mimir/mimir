#pragma once

#include "mim/phase.h"

namespace mim {

/// Eliminates all Single%ton types, so that no Single/Wrap ever reaches a backend.
/// A singleton carries no information, so its erasure is the unit: `«e»` becomes `[]` and `‹e›` becomes `()`.
/// Within a Sigma the component is dropped altogether, which shifts the indices of its successors.
/// A singleton above `*` has no unit to erase to and degrades to the type of its inhabitant instead.
class SingleElim : public RWPhase {
public:
    SingleElim(World& world, std::string name)
        : RWPhase(world, std::move(name)) {}
    SingleElim(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// @name Erasure
    /// All arguments live in old_world(), all results in new_world().
    ///@{
    bool is_unit(const Single* single) { return single->type() == old_world().type<0>(); }
    const Def* erase(const Single*);      ///< The erasure of the singleton *type* itself.
    const Def* inhabitant(const Single*); ///< The erasure of its sole inhabitant.
    static bool is_gone(const Def* type) { return type->isa<Single>(); }
    static size_t num_gone(const Sigma*);
    /// The literal @p index into @p sigma, adjusted for its dropped components.
    size_t remap(const Sigma* sigma, const Def* index);
    ///@}

    const Def* rewrite_imm_Single(const Single*) final;
    const Def* rewrite_imm_Wrap(const Wrap*) final;
    const Def* rewrite_imm_Sigma(const Sigma*) final;
    const Def* rewrite_mut_Sigma(Sigma*) final;
    const Def* rewrite_imm_Tuple(const Tuple*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_imm_Insert(const Insert*) final;
    const Def* rewrite_imm_Seq(const Seq*) final;
    const Def* rewrite_mut_Seq(Seq*) final;
};

} // namespace mim
