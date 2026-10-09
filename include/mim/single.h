#pragma once

#include <span>

#include "mim/def.h"

namespace mim {

/// Single%ton type formation.
class Single : public Def, public Setters<Single> {
private:
    Single(const Def* type, const Def* op)
        : Def(Node, type, {op}, 0) {}

public:
    using Setters<Single>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); }
    ///@}

    static constexpr auto Node      = mim::Node::Single;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

/// Single%ton term introduction.
/// @note We never build a singleton term elimination as World::unwrap immediately normalizes to Single::op.
class Wrap : public Def, public Setters<Wrap> {
private:
    Wrap(const Def* type, const Def* op)
        : Def(Node, type, {op}, 0) {}

public:
    using Setters<Wrap>::set;

    /// @name ops
    ///@{
    const Def* op() const { return Def::op(0); }
    ///@}

    static constexpr auto Node      = mim::Node::Wrap;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
};

} // namespace mim
