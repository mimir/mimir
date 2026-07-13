#pragma once

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>

#include "mim/plug/mem/autogen.h"

namespace mim::plug::mem {

/// @name %%mem.M
///@{

inline Lam* mut_con(World& w, nat_t a = 0) { return w.mut_con(w.call<M>(a)); } ///< Yields `con[%mem.M 0]`.

/// Yields `con[%mem.M 0, dom]`.
inline Lam* mut_con(const Def* dom) {
    World& w = dom->world();
    return w.mut_con({w.call<M>(0), dom});
}

/// Returns the (first) element of type `%mem.M a` from the given tuple.
inline const Def* mem_def(const Def* def) {
    if (Axm::isa<mem::M>(def->type())) return def;
    if (def->type()->isa<Arr>()) return {}; // don't look into possibly gigantic arrays

    if (def->num_projs() > 1) {
        for (auto proj : def->projs())
            if (auto mem = mem_def(proj)) return mem;
    }

    return {};
}

/// Returns the memory argument of a function if it has one.
inline const Def* mem_var(Lam* lam) { return mem_def(lam->var()); }

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

/// Recursively removes all occurrences of mem from a tuple.
/// Returns an empty tuple if applied with mem alone.
inline const Def* strip_mem(const Def* def) {
    auto& world = def->world();

    if (auto tuple = def->isa<Tuple>()) {
        DefVec new_ops;
        for (auto op : tuple->ops())
            if (auto new_op = strip_mem(op); new_op != world.tuple()) new_ops.push_back(new_op);

        return world.tuple(new_ops);
    } else if (Axm::isa<mem::M>(def->type())) {
        return world.tuple();
    } else if (auto extract = def->isa<Extract>()) {
        // The case that this one element is a mem and should return () is handled above.
        if (extract->num_projs() == 1) return extract;

        DefVec new_ops;
        for (auto op : extract->projs())
            if (auto new_op = strip_mem(op); new_op != world.tuple()) new_ops.push_back(new_op);

        return world.tuple(new_ops);
    }

    return def;
}
///@}

/// @name %%mem.lea
///@{
inline const Def* pointee(const Def* ptr) {
    auto [pointee, _] = Axm::as<Ptr>(ptr->type())->args<2>();
    return pointee;
}
///@}

/// @name %%mem.lea
///@{
inline const Def* op_lea(const Def* ptr, const Def* index) {
    World& w                   = ptr->world();
    auto [pointee, addr_space] = Axm::as<Ptr>(ptr->type())->args<2>();
    auto Ts                    = tuple_of_types(pointee);
    return w.app(w.app(w.annex<lea>(), {pointee->arity(), Ts, addr_space}), {ptr, index});
}

inline const Def* op_lea_unsafe(const Def* ptr, const Def* i) {
    World& w = ptr->world();
    return op_lea(ptr, w.call(core::conv::u, Axm::as<Ptr>(ptr->type())->arg(0)->arity(), i));
}

inline const Def* op_lea_unsafe(const Def* ptr, u64 i) { return op_lea_unsafe(ptr, ptr->world().lit_i64(i)); }
///@}

/// @name %%mem.alloc
///@{
inline const Def* op_alloc(const Def* type, const Def* as, const Def* mem) {
    World& w = type->world();
    return w.app(w.app(w.annex<alloc>(), {type, as}), mem);
}
inline const Def* op_alloc(const Def* type, const Def* mem) { return op_alloc(type, type->world().lit_nat_0(), mem); }
///@}

/// @name %%mem.slot
///@{
inline const Def* op_slot(const Def* type, const Def* as, const Def* mem) {
    World& w = type->world();
    return w.app(w.app(w.annex<slot>(), {type, as}), {mem, w.lit_nat(w.curr_gid())});
}
inline const Def* op_slot(const Def* type, const Def* mem) { return op_slot(type, type->world().lit_nat_0(), mem); }
///@}

/// @name %%mem.malloc
///@{
inline const Def* op_malloc(const Def* type, const Def* as, const Def* mem) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<malloc>(), {type, as}), {mem, size});
}
inline const Def* op_malloc(const Def* type, const Def* mem) { return op_malloc(type, type->world().lit_nat_0(), mem); }
///@}

/// @name %%mem.mslot
///@{
inline const Def* op_mslot(const Def* type, const Def* as, const Def* mem, const Def* id) {
    World& w  = type->world();
    auto size = w.call(core::trait::size, type);
    return w.app(w.app(w.annex<mslot>(), {type, as}), {mem, size, id});
}
inline const Def* op_mslot(const Def* type, const Def* mem, const Def* id) {
    return op_mslot(type, type->world().lit_nat_0(), mem, id);
}
///@}

} // namespace mim::plug::mem
