#pragma once

#include "mim/phase.h"

namespace mim {

/// Eliminates all types that have exactly one inhabitant, so that no Single/Wrap ever reaches a backend.
/// Such a type carries no information, so its erasure is the unit: `«e»` becomes `[]` and `‹e›` becomes `()`.
/// Within an aggregate the component is dropped altogether, which shifts the indices of its successors.
/// Besides Single this also catches the unit `[]` and `Idx 1` - and any aggregate built from those.
/// A singleton above `*` has no unit to erase to and degrades to the type of its inhabitant instead.
/// An aggregate whose arity another def relies upon keeps all of its components; only the erasure is mandatory.
class SingleErasure : public RWPhase {
private:
    /// Pins the aggregates that must keep their arity.
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "SingleErasure::Analysis") {}

        bool pinned(const Def* type) const { return is_top(type); }

    private:
        const Def* rewrite(const Def* old) final;
        void pin_tree(const Def* def, DefSet& visited); ///< pin%s every Sigma / Arr nested in @p def.
    };

public:
    SingleErasure(World& world, std::string name)
        : RWPhase(world, std::move(name), &analysis_)
        , analysis_(world) {}
    SingleErasure(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    /// @name Erasure
    /// All arguments live in old_world(), all results in new_world().
    ///@{
    bool is_unit(const Single* single) { return single->type() == old_world().type<0>(); }
    const Def* erase(const Single*);        ///< The erasure of the singleton *type* itself.
    const Def* inhabitant(const Def* type); ///< The sole inhabitant of the information-free @p type.
    bool is_gone(const Def* type);          ///< Does @p type have exactly one inhabitant?
    Sieve sieve(const Sigma*);              ///< The components of @p sigma that survive.
    /// The literal @p index into a Sigma, adjusted for the components @p keep dropped.
    size_t remap(const Sieve& keep, const Def* index);
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

    Analysis analysis_;
};

} // namespace mim
