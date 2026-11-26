#pragma once

#include "mim/def.h"

namespace mim {

/// The type of a rewrite Rule that rewrites a term of type Rho::dom into one of type Rho::codom.
/// Its Var is that term.
/// @see Rule
class Rho : public Def, public Setters<Rho> {
protected:
    /// Constructor for an *immutable* Rho.
    Rho(const Def* type, const Def* dom, const Def* codom)
        : Def(Node, type, {dom, codom}, 0) {}
    /// Constructor for a *mutable* Rho.
    Rho(const Def* type)
        : Def(Node, type, 2, 0) {}

public:
    /// @name dom & codom
    /// @see @ref proj
    ///@{
    const Def* dom() const { return op(0); }
    const Def* codom() const { return op(1); }
    MIM_PROJ(dom, const)
    MIM_PROJ(codom, const)
    ///@}

    /// @name Setters
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Rho>::set;
    Rho* set(const Def* dom, const Def* codom) { return Def::set({dom, codom})->as<Rho>(); }
    Rho* set_dom(const Def* dom) { return Def::set(0, dom)->as<Rho>(); }
    Rho* set_codom(const Def* codom) { return Def::set(1, codom)->as<Rho>(); }
    Rho* unset() { return Def::unset()->as<Rho>(); }
    ///@}

    /// @name Type Checking
    ///@{
    static const Def* infer(const Def* dom, const Def* codom);
    ///@}

    /// @name Reduce
    ///@{
    const Def* reduce(const Def* arg) const { return Def::reduce(arg).front(); }
    ///@}

    static constexpr auto Node      = mim::Node::Rho;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

/// A rewrite rule `rl (meta): lhs when guard => rhs`; its Var binds the meta variables, which matching determines.
/// A family of rules abstracts over further parameters by ordinary Lam%s around it.
/// @see Rho, Fire
class Rule : public Def, public Setters<Rule> {
private:
    Rule(const Rho* rho, const Def* meta, const Def* lhs, const Def* guard, const Def* rhs)
        : Def(Node, rho, {meta, lhs, guard, rhs}, 0) {}
    Rule(const Rho* rho)
        : Def(Node, rho, 4, 0) {}

public:
    /// @name ops
    /// @see @ref proj
    ///@{
    const Def* meta() const { return op(0); } ///< The type of the meta variables.
    const Def* lhs() const { return op(1); }
    const Def* guard() const { return op(2); }
    const Def* rhs() const { return op(3); }
    MIM_PROJ(meta, const)
    ///@}

    /// @name type
    /// @see @ref proj
    ///@{
    const Rho* type() const { return Def::type()->as<Rho>(); }
    const Def* dom() const { return type()->dom(); }
    const Def* codom() const { return type()->codom(); }
    MIM_PROJ(dom, const)
    MIM_PROJ(codom, const)
    ///@}

    /// @name Setters
    /// Set Rule::meta first, as it is the type of the Var the others may use.
    /// @see @ref set_ops "Setting Ops"
    ///@{
    using Setters<Rule>::set;
    Rule* set(const Def* meta, const Def* lhs, const Def* guard, const Def* rhs) {
        return Def::set({meta, lhs, guard, rhs})->as<Rule>();
    }
    Rule* set_meta(const Def* meta) { return Def::set(0, meta)->as<Rule>(); }
    Rule* set_lhs(const Def* lhs) { return Def::set(1, lhs)->as<Rule>(); }
    Rule* set_guard(const Def* guard) { return Def::set(2, guard)->as<Rule>(); }
    Rule* set_rhs(const Def* rhs) { return Def::set(3, rhs)->as<Rule>(); }
    Rule* set(Defs ops) { return Def::set(ops)->as<Rule>(); }
    Rule* unset() { return Def::unset()->as<Rule>(); }
    ///@}

    /// @name Type Checking
    ///@{
    static const Def* check_guard(const Def* guard);
    ///@}

    /// @name Fire
    ///@{
    /// Rewrites @p arg, if it matches Rule::lhs and Rule::guard holds; `nullptr` otherwise.
    const Def* fire(const Def* arg) const;
    ///@}

    static constexpr auto Node      = mim::Node::Rule;
    static constexpr size_t Num_Ops = 4;

private:
    friend class World;
};

/// Applies the Rule Fire::callee to Fire::arg.
/// It stays as long as the Rule does not match.
/// @see Rule
class Fire : public Def, public Setters<Fire> {
private:
    Fire(const Def* type, const Def* callee, const Def* arg)
        : Def(Node, type, {callee, arg}, 0) {}

public:
    using Setters<Fire>::set;

    /// @name callee
    ///@{
    const Def* callee() const { return op(0); }
    ///@}

    /// @name arg
    /// @see @ref proj
    ///@{
    const Def* arg() const { return op(1); }
    MIM_PROJ(arg, const)
    ///@}

    static constexpr auto Node      = mim::Node::Fire;
    static constexpr size_t Num_Ops = 2;

private:
    friend class World;
};

} // namespace mim
