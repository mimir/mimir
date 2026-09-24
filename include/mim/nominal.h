#pragma once

#include "mim/def.h"

namespace mim {

/// Nominal newtype formation.
/// Def::flags are the annex flags of the `nom` declaration that introduced it, so two `nom`s over the same
/// Nom::op are still distinct types - this is the one place where MimIR is not structurally typed.
class Nom : public Def, public Setters<Nom> {
private:
    Nom(const Def* type, const Def* op, flags_t flags)
        : Def(Node, type, {op}, flags) {}

public:
    using Setters<Nom>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); } ///< The wrapped type.
    ///@}

    static constexpr auto Node      = mim::Node::Nom;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// Nominal newtype term introduction.
class Wrap : public Def, public Setters<Wrap> {
private:
    Wrap(const Def* type, const Def* value)
        : Def(Node, type, {value}, 0) {}

public:
    using Setters<Wrap>::set;

    /// @name ops
    ///@{
    const Def* value() const { return op(0); }
    const Nom* nom() const { return type()->as<Nom>(); }
    ///@}

    static constexpr auto Node      = mim::Node::Wrap;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// Nominal newtype term elimination.
class Unwrap : public Def, public Setters<Unwrap> {
private:
    Unwrap(const Def* type, const Def* value)
        : Def(Node, type, {value}, 0) {}

public:
    using Setters<Unwrap>::set;

    /// @name ops
    ///@{
    const Def* value() const { return op(0); }
    ///@}

    static constexpr auto Node      = mim::Node::Unwrap;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

} // namespace mim
