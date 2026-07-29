#pragma once

#include "mim/world.h"

#include "mim/plug/clos/autogen.h"
#include "mim/plug/mem/autogen.h"

namespace mim::plug::clos {

/// @name Closures
///@{

/// Lightweight, non-owning view onto a closure literal `(env_type, fn, env)`; see isa_clos_lit.
class ClosLit {
public:
    /// @name Getters
    ///@{
    const Sigma* type() const { return def_->type()->isa<Sigma>(); }

    const Def* env() const;
    const Def* env_type() const { return env()->type(); }

    const Def* fnc() const;
    const Pi* fnc_type() const { return fnc()->type()->isa<Pi>(); }
    Lam* fnc_as_lam() const;

    const Def* env_var() const;
    const Def* ret_var() const { return fnc_as_lam()->ret_var(); }
    ///@}

    /// @name Predicates
    ///@{
    explicit operator bool() const { return def_ != nullptr; }
    operator const Tuple*() const { return def_; }
    const Tuple* operator->() const { return def_; }

    bool is_returning() const { return Pi::isa_returning(fnc_type()); }
    bool is_basicblock() const { return Pi::isa_basicblock(fnc_type()); }
    ///@}

private:
    explicit ClosLit(const Tuple* def)
        : def_(def) {}

    const Tuple* def_;

    friend ClosLit isa_clos_lit(const Def*, bool);
};

/// Tries to match a closure literal.
/// If @p fn_isa_lam, additionally requires the code part to be a Lam.
ClosLit isa_clos_lit(const Def* def, bool fn_isa_lam = true);

/// Pack a typed closure.
/// This assumes that @p fn expects the environment at its env_param()%th argument.
const Def* clos_pack(const Def* env, const Def* fn, const Def* ct = nullptr);

/// Deconstruct a closure into `(env_type, function, env)`.
/// **Important**: use this or ClosLit to destruct closures, since typechecking dependent pairs is currently
/// broken.
std::tuple<const Def*, const Def*, const Def*> clos_unpack(const Def* c);

/// Apply a closure to arguments.
const Def* clos_apply(const Def* closure, const Def* args);
inline const Def* apply_closure(const Def* closure, Defs args) {
    return clos_apply(closure, closure->world().tuple(args));
}

/// If @p def is a projection `var#i` of the Var of some mutable of type @p N, returns `(projection, binder)`.
/// Otherwise, returns `(nullptr, nullptr)`.
template<class N>
std::tuple<const Extract*, N*> isa_var_proj(const Def* def) {
    if (auto proj = def->isa<Extract>())
        if (auto var = proj->tuple()->isa<Var>(); var && var->binder()->isa<N>()) return {proj, var->binder()->as<N>()};
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

/// Describes where the environment is placed in the argument list: right after a leading `%mem.M`, if @p doms
/// starts with one, or in slot 0 otherwise. This way, closures built from mem-free (pure) functions don't gain
/// a bogus mem-shaped layout, and don't get misaligned with their real parameters (see issue #126).
inline size_t env_param(Defs doms) { return (!doms.empty() && Axm::isa<mem::M>(doms.front())) ? 1_u64 : 0_u64; }
inline size_t env_param(const Pi* pi) { return env_param(pi->doms()); }

/// Adjust the index of an argument to account for the env param.
inline size_t shift_env(size_t ep, size_t i) { return (i < ep) ? i : i - 1_u64; }

/// Same as shift_env, but skips the env param instead.
inline size_t skip_env(size_t ep, size_t i) { return (i < ep) ? i : i + 1_u64; }

/// Builds a closure type from the domains @p doms of a `Cn`.
/// If @p env_type is `nullptr`, returns the recursive closure Sigma `[T: *, Cn [doms with T at env_param], T]`.
/// Otherwise, returns the bare `Cn [doms with env_type at env_param]` (the code part of such a closure).
const Def* ctype(World& w, Defs doms, const Def* env_type = nullptr);

const Def* clos_insert_env(size_t ep, size_t i, const Def* env, std::function<const Def*(size_t)> f);
inline const Def* clos_insert_env(size_t ep, size_t i, const Def* env, const Def* a) {
    return clos_insert_env(ep, i, env, [&](auto i) { return a->proj(i); });
}

inline const Def* clos_insert_env(size_t ep, const Def* env, const Def* tup_or_sig) {
    auto& w      = tup_or_sig->world();
    auto new_ops = DefVec(tup_or_sig->num_projs() + 1, [&](auto i) { return clos_insert_env(ep, i, env, tup_or_sig); });
    return (tup_or_sig->isa<Sigma>()) ? w.sigma(new_ops) : w.tuple(new_ops);
}

const Def* clos_remove_env(size_t ep, size_t i, std::function<const Def*(size_t)> f);
inline const Def* clos_remove_env(size_t ep, size_t i, const Def* def) {
    return clos_remove_env(ep, i, [&](auto i) { return def->proj(i); });
}
inline const Def* clos_remove_env(size_t ep, const Def* tup_or_sig) {
    auto& w      = tup_or_sig->world();
    auto new_ops = DefVec(tup_or_sig->num_projs() - 1, [&](auto i) { return clos_remove_env(ep, i, tup_or_sig); });
    return (tup_or_sig->isa<Sigma>()) ? w.sigma(new_ops) : w.tuple(new_ops);
}

inline const Def* clos_sub_env(size_t ep, const Def* tup_or_sig, const Def* new_env) {
    return tup_or_sig->refine(ep, new_env);
}
///@}

} // namespace mim::plug::clos
