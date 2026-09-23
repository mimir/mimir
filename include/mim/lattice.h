#pragma once

#include <span>

#include "mim/def.h"

namespace mim {

/// Common base for TExt%remum.
class Ext : public Def {
protected:
    Ext(Node node, const Def* type)
        : Def(node, type, Defs{}, 0) {}

public:
    /// Ext groups Top and Bot; see fe::NodeSetable.
    static constexpr bool isa_node(mim::Node n) noexcept { return n == mim::Node::Top || n == mim::Node::Bot; }
};

/// Ext%remum. Either Top (@p Up) or Bot%tom.
template<bool Up>
class TExt : public Ext, public Setters<TExt<Up>> {
private:
    TExt(const Def* type)
        : Ext(Node, type) {}

public:
    using Setters<TExt<Up>>::set;

    static constexpr auto Node      = Up ? mim::Node::Top : mim::Node::Bot;
    static constexpr size_t Num_Ops = 0;

private:
    friend class World;
};

/// @name Lattice
///@{
using Bot = TExt<false>;
using Top = TExt<true>;
/// @}

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
/// @note We never build a singleton term elimantoin as World::unwrap immediately normalizes to Single::op.
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
