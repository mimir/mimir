#pragma once

#include "mim/def.h"

namespace mim {

/// Nominal newtype formation.
/// Two Nom%s are only equal if their Nom::key%s are, so two Nom%s over the same Nom::op may still be distinct types.
/// It is up to the frontend to hand out a fresh Nom::key per declaration; Def::sym is mere debug info.
/// A recursive Nom refers to itself through a mutable Nom::op.
class Nom : public Def, public Setters<Nom> {
private:
    Nom(const Def* type, const Def* op, flags_t key)
        : Def(Node, type, {op}, key) {}

public:
    using Setters<Nom>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); } ///< The wrapped type.
    ///@}

    flags_t key() const { return flags(); } ///< What tells this Nom apart from one over the same Nom::op.

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
