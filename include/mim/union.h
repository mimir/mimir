#pragma once

#include <span>

#include "mim/def.h"

namespace mim {

/// A union type whose ops are sorted and deduplicated: `T ∪ T` is `T`.
class Join : public Def, public Setters<Join> {
private:
    Join(const Def* type, Defs ops)
        : Def(Node, type, ops, 0) {}

public:
    using Setters<Join>::set;

    static constexpr auto Node      = mim::Node::Join;
    static constexpr size_t Num_Ops = std::dynamic_extent;

private:
    friend class World;
};

/// Constructs a [Join](@ref mim::Join) or Variant **value**.
class Inj : public Def, public Setters<Inj> {
private:
    Inj(const Def* type, const Def* value, flags_t index)
        : Def(Node, type, {value}, index) {}

public:
    using Setters<Inj>::set;

    /// @name ops
    ///@{
    const Def* value() const { return op(0); }
    ///@}

    /// The case of a Variant this injects into.
    /// A Join has no use for it: there, value()'s type determines the case.
    size_t index() const { return flags(); }

    static constexpr auto Node      = mim::Node::Inj;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// A sum type whose cases are positional: unlike a Join, they are neither sorted nor deduplicated.
/// It is *mutable* only if it refers to itself.
class Variant : public Def, public Setters<Variant> {
private:
    Variant(const Def* type, Defs ops)
        : Def(Node, type, ops, 0) {} ///< Constructor for an *immutable* Variant.
    Variant(const Def* type, size_t size)
        : Def(Node, type, size, 0) {} ///< Constructor for a *mutable* Variant.

public:
    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Variant>::set;
    Variant* set(size_t i, const Def* def) { return Def::set(i, def)->as<Variant>(); }
    Variant* set(Defs ops) { return Def::set(ops)->as<Variant>(); }
    Variant* unset() { return Def::unset()->as<Variant>(); }
    ///@}

    /// @name Type Checking
    ///@{
    static const Def* infer(World&, Defs);
    ///@}

    static constexpr auto Node      = mim::Node::Variant;
    static constexpr size_t Num_Ops = std::dynamic_extent;

private:
    friend class World;
};

/// Scrutinize Match::scrutinee() and dispatch to Match::arms.
/// For a Variant, the arms are positional: `arm(i)` handles case `i`.
class Match : public Def, public Setters<Match> {
private:
    Match(const Def* type, Defs ops)
        : Def(Node, type, ops, 0) {}

public:
    using Setters<Match>::set;
    static constexpr auto Node      = mim::Node::Match;
    static constexpr size_t Num_Ops = std::dynamic_extent;

    /// @name ops
    ///@{
    const Def* scrutinee() const { return op(0); }
    template<size_t N = std::dynamic_extent>
    constexpr auto arms() const noexcept {
        return ops().subspan<1, N>();
    }
    const Def* arm(size_t i) const { return arms()[i]; }
    size_t num_arms() const { return arms().size(); } ///< @warning Can undercut Join::num_ops!
    ///@}

    /// @name Dispatch
    ///@{
    /// The cases @p scrutinee dispatches on: its Join's or Variant's ops, or - a one-case union - its type.
    static DefVec cases(const Def* scrutinee);
    /// Does @p arm handle @p c? An arm accepts the case that *is* its domain, or that its domain contains.
    /// @p infer resolves Hole%s, so a mere search should leave it `false`.
    static bool accepts(const Def* arm, const Def* c, bool infer = false);
    ///@}

private:
    friend class World;
};

} // namespace mim
