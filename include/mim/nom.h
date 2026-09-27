#pragma once

#include "mim/def.h"

namespace mim {

/// Nominal newtype formation.
/// A Nom is a mutable without Var that never immutabilizes, so two Nom%s over the same Nom::op are still distinct
/// types - this is the one place where MimIR is not structurally typed.
class Nom : public Def, public Setters<Nom> {
private:
    Nom(const Def* type)
        : Def(Node, type, 1, 0) {}

public:
    using Setters<Nom>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); } ///< The wrapped type.
    Nom* set(const Def* op) { return Def::set(0, op)->as<Nom>(); }
    ///@}

    static constexpr auto Node      = mim::Node::Nom;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// Nominal newtype term introduction.
class Name : public Def, public Setters<Name> {
private:
    Name(const Def* type, const Def* value)
        : Def(Node, type, {value}, 0) {}

public:
    using Setters<Name>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); }
    const Nom* nom() const { return type()->as<Nom>(); }
    ///@}

    static constexpr auto Node      = mim::Node::Name;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// Nominal newtype term elimination.
class Struc : public Def, public Setters<Struc> {
private:
    Struc(const Def* type, const Def* value)
        : Def(Node, type, {value}, 0) {}

public:
    using Setters<Struc>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); }
    ///@}

    static constexpr auto Node      = mim::Node::Struc;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

} // namespace mim
