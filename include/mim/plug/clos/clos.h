#pragma once

#include "mim/world.h"

#include "mim/plug/clos/autogen.h"

namespace mim::plug::clos {

/// @name Closures
///@{

/// Wrapper around a Def that can be used to match closures (see isa_clos_lit).
class ClosLit {
public:
    /// @name Getters
    ///@{
    const Sigma* type() {
        assert(def_);
        return def_->type()->isa<Sigma>();
    }

    const Def* env();
    const Def* env_type() { return env()->type(); }

    const Def* fnc();
    const Pi* fnc_type() { return fnc()->type()->isa<Pi>(); }
    Lam* fnc_as_lam();

    const Def* env_var();
    const Def* ret_var() { return fnc_as_lam()->ret_var(); }
    ///@}

    explicit operator bool() const { return def_ != nullptr; }
    operator const Tuple*() { return def_; }

    const Tuple* operator->() {
        assert(def_);
        return def_;
    }

    /// @name Properties
    ///@{
    bool is_returning() { return Pi::isa_returning(fnc_type()); }
    bool is_basicblock() { return Pi::isa_basicblock(fnc_type()); }
    attr get() { return attr_; } ///< Clos annotation. These should appear in front of the code-part.
    ///@}

private:
    ClosLit(const Tuple* def, attr a = attr::bottom)
        : def_(def)
        , attr_(a) {}

    const Tuple* def_;
    const attr attr_;

    friend ClosLit isa_clos_lit(const Def*, bool);
};

/// Tries to match a closure literal.
ClosLit isa_clos_lit(const Def* def, bool fn_isa_lam = true);

/// Pack a typed closure. This assumes that @p fn expects the environment as its Clos_Env_Param%th argument.
const Def* clos_pack(const Def* env, const Def* fn, const Def* ct = nullptr);

/// Deconstruct a closure into `(env_type, function, env)`.
/// **Important**: use this or ClosLit to destruct closures, since typechecking dependent pairs is currently
/// broken.
std::tuple<const Def*, const Def*, const Def*> clos_unpack(const Def* c);

/// Apply a closure to arguments.
const Def* clos_apply(const Def* closure, const Def* args);
inline const Def* apply_closure(const Def* closure, Defs args) {
    auto& w = closure->world();
    return clos_apply(closure, w.tuple(args));
}

// TODO: rename this
/// Checks if def is the variable of a mut of type N.
template<class N> std::tuple<const Extract*, N*> ca_isa_var(const Def* def) {
    if (auto proj = def->isa<Extract>()) {
        if (auto var = proj->tuple()->isa<Var>(); var && var->mut()->isa<N>())
            return std::tuple(proj, var->mut()->as<N>());
    }
    return {nullptr, nullptr};
}
///@}

/// @name Closure Types
///@{
/// Returns @p def if @p def is a closure and @c nullptr otherwise
const Sigma* isa_clos_type(const Def* def);

/// Creates a typed closure type from @p pi.
Sigma* clos_type(const Pi* pi);

/// Convert a closure type to a Pi, where the environment type has been removed or replaced by @p new_env_type
/// (if @p new_env_type != @c nullptr)
const Pi* clos_type_to_pi(const Def* ct, const Def* new_env_type = nullptr);

///@}

/// @name Closure Environment
///@{
/// `tup_or_sig` should generally be a Tuple, Sigma or Var.

/// Describes where the environment is placed in the argument list.
static constexpr size_t Clos_Env_Param = 1_u64;

// Adjust the index of an argument to account for the env param
inline size_t shift_env(size_t i) { return (i < Clos_Env_Param) ? i : i - 1_u64; }

// Same but skip the env param
inline size_t skip_env(size_t i) { return (i < Clos_Env_Param) ? i : i + 1_u64; }

// TODO what does this do exactly?
const Def* ctype(World& w, Defs doms, const Def* env_type = nullptr);

const Def* clos_insert_env(size_t i, const Def* env, std::function<const Def*(size_t)> f);
inline const Def* clos_insert_env(size_t i, const Def* env, const Def* a) {
    return clos_insert_env(i, env, [&](auto i) { return a->proj(i); });
}

inline const Def* clos_insert_env(const Def* env, const Def* tup_or_sig) {
    auto& w      = tup_or_sig->world();
    auto new_ops = DefVec(tup_or_sig->num_projs() + 1, [&](auto i) { return clos_insert_env(i, env, tup_or_sig); });
    return (tup_or_sig->isa<Sigma>()) ? w.sigma(new_ops) : w.tuple(new_ops);
}

const Def* clos_remove_env(size_t i, std::function<const Def*(size_t)> f);
inline const Def* clos_remove_env(size_t i, const Def* def) {
    return clos_remove_env(i, [&](auto i) { return def->proj(i); });
}
inline const Def* clos_remove_env(const Def* tup_or_sig) {
    auto& w      = tup_or_sig->world();
    auto new_ops = DefVec(tup_or_sig->num_projs() - 1, [&](auto i) { return clos_remove_env(i, tup_or_sig); });
    return (tup_or_sig->isa<Sigma>()) ? w.sigma(new_ops) : w.tuple(new_ops);
}

inline const Def* clos_sub_env(const Def* tup_or_sig, const Def* new_env) {
    return tup_or_sig->refine(Clos_Env_Param, new_env);
}
///@}

} // namespace mim::plug::clos
