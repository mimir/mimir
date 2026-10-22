#pragma once

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>

#include "mim/plug/mem/autogen.h"

namespace mim::plug::mem {

/// @name mem.M
///@{

inline Lam* mut_con(World& w, nat_t a = 0) { return w.mut_con(w.call<M>(a)); } ///< Yields `con[mem.M 0]`.

/// Yields `con[mem.M 0, dom]`.
inline Lam* mut_con(const Def* dom) {
    World& w = dom->world();
    return w.mut_con({w.call<M>(0), dom});
}

/// If @p def is a `mem.M`-typed value, yields its memory type `mem.M a`; otherwise `nullptr`.
inline const App* isa_mem(const Def* def) { return Axm::isa<mem::M>(def->type()); }

/// Does @p pi already thread memory - a leading `mem.M`, either directly or grouped as the first
/// component of the first parameter (e.g. the `Fn [mem.M 0, To, ins] → …` shape of a mem-threaded combiner)?
inline bool has_leading_mem(const Pi* pi) {
    if (pi->num_doms() == 0) return false;
    auto dom0 = pi->dom(pi->num_doms(), 0);
    if (Axm::isa<mem::M>(dom0)) return true;
    if (auto sig = dom0->isa<Sigma>(); sig && sig->num_ops() != 0 && Axm::isa<mem::M>(sig->op(0))) return true;
    return false;
}

/// Returns the (first) element of type `mem.M a` from the given tuple.
inline const Def* mem_def(const Def* def) {
    if (isa_mem(def)) return def;
    if (def->isa<Arr>() || def->type()->isa<Arr>()) return {}; // don't look into possibly gigantic arrays

    if (def->num_projs() > 1) {
        for (auto proj : def->projs())
            if (auto mem = mem_def(proj)) return mem;
    }

    return {};
}

/// Returns the memory argument of a function if it has one.
inline const Def* mem_var(Lam* lam) { return mem_def(lam->var()); }

/// Is @p type `mem.M` or a Sigma with a `mem.M` component?
inline bool has_mem_ty(const Def* type) {
    if (Axm::isa<mem::M>(type)) return true;
    if (auto sigma = type->isa<Sigma>())
        return std::ranges::any_of(sigma->ops(), [](const Def* op) { return Axm::isa<mem::M>(op); });
    return false;
}

/// Removes recusively all occurences of mem from a type (sigma).
inline const Def* strip_mem_ty(const Def* def) {
    auto& world = def->world();

    if (auto sigma = def->isa<Sigma>()) {
        DefVec new_ops;
        for (auto op : sigma->ops())
            if (auto new_op = strip_mem_ty(op); new_op != world.sigma()) new_ops.push_back(new_op);

        return world.sigma(new_ops);
    } else if (Axm::isa<mem::M>(def)) {
        return world.sigma();
    }

    return def;
}
///@}

/// @name mem.Ptr
///@{
inline const Def* pointee(const Def* ptr) {
    auto [pointee, _] = Axm::as<Ptr>(ptr->type())->args<2>();
    return pointee;
}
///@}

/// @name mem.lea
///@{
inline const Def* op_lea(const Def* ptr, const Def* index) {
    World& w                   = ptr->world();
    auto [pointee, addr_space] = Axm::as<Ptr>(ptr->type())->args<2>();
    auto Ts                    = tuple_of_types(pointee);
    return w.app(w.app(w.annex<lea>(), {pointee->arity(), Ts, addr_space}), {ptr, index});
}

/// Successively offsets @p ptr by each component of the index @p tuple, peeling one array dimension per `mem.lea`.
inline const Def* op_lea_tuple(const Def* ptr, const Def* tuple) {
    auto n       = tuple->num_projs();
    auto element = ptr;
    for (size_t i = 0; i != n; ++i)
        element = op_lea(element, tuple->proj(n, i));
    return element;
}
///@}

/// @name mem.alloc
///@{
inline const Def* op_alloc(const Def* type, const Def* as, const Def* mem) {
    World& w = type->world();
    return w.app(w.app(w.annex<alloc>(), {type, as}), mem);
}
inline const Def* op_alloc(const Def* type, const Def* mem) { return op_alloc(type, type->world().lit_nat_0(), mem); }
///@}

/// @name mem.slot
///@{
/// Builds the continuation-based `mem.slot (type, as)` applied to @p mem and the continuation @p ret.
/// @p ret is a `Cn [mem.M as, mem.Ptr (type, as)]` receiving the fresh slot; its scope delimits the slot's lifetime.
inline const Def* op_slot(const Def* type, const Def* as, const Def* mem, const Def* ret) {
    World& w = type->world();
    return w.app(w.app(w.annex<slot>(), {type, as}), {mem, ret});
}
/// Decomposes the continuation-based @p slot into `(mem, ret_lam, ret_mem, ptr)`,
/// where `ret_mem` is the continuation's mem and `ptr` is the continuation's slot var;
/// the slot's lifetime is `ret_lam`'s scope.
/// All but `mem` are `nullptr` if the continuation is no mutable Lam.
inline std::tuple<const Def*, Lam*, const Def*, const Def*> split_slot(const App* slot) {
    auto [mem, ret] = slot->args<2>();
    if (auto ret_lam = ret->isa_mut<Lam>()) return {mem, ret_lam, ret_lam->var(2, 0), ret_lam->var(2, 1)};
    return {mem, nullptr, nullptr, nullptr};
}

///@}

/// @name mem.malloc
///@{
inline const Def* op_malloc(const Def* type, const Def* as, const Def* mem) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<malloc>(), {type, as}), {mem, size});
}
///@}

/// @name mem.mslot
///@{
/// Builds the continuation-based `mem.mslot (type, as)` applied to `(mem, size)` and the continuation @p ret.
/// @p ret is a `Cn [mem.M as, mem.Ptr (type, as)]` receiving the fresh slot; its scope delimits the slot's lifetime.
inline const Def* op_mslot(const Def* type, const Def* as, const Def* mem, const Def* ret) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<mslot>(), {type, as}), {w.tuple({mem, size}), ret});
}
///@}

} // namespace mim::plug::mem
