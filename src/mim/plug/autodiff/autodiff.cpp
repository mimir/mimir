#include "mim/plug/autodiff/autodiff.h"

#include <mim/config.h>
#include <mim/phase.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/autodiff/pass/eval.h"

using namespace std::literals;
using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    Stage::hook<autodiff::eval_pass, autodiff::Eval>(stages);

    MIM_REPL(stages, autodiff::zero_repl, {
        if (auto zero = Axm::isa<autodiff::zero>(def); zero) {
            if (auto z = autodiff::zero_def(zero->arg())) return z;
        }
        return {};
    });
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"autodiff", MIM_VERSION, [](Normalizers& n) { autodiff::register_normalizers(n); }, reg_stages, nullptr};
}

namespace mim::plug::autodiff {
const Def* id_pullback(const Def* A) {
    auto& world       = A->world();
    auto arg_pb_ty    = pullback_type(A, A);
    auto id_pb        = world.mut_lam(arg_pb_ty)->set("id_pb");
    auto id_pb_scalar = id_pb->var(0uz)->set("s");
    id_pb->app(true,
               id_pb->var(1), // can not use ret_var as the result might be higher order
               id_pb_scalar);

    return id_pb;
}

const Def* zero_pullback(const Def* E, const Def* A) {
    auto& world    = A->world();
    auto A_tangent = tangent_type_fun(A);
    auto pb_ty     = pullback_type(E, A);
    auto pb        = world.mut_lam(pb_ty)->set("zero_pb");
    world.DLOG("zero_pullback for {} resp. {} (-> {})", E, A, A_tangent);
    pb->app(true, pb->var(1), world.call<zero>(A_tangent));
    return pb;
}

//  `P` => `P*`
//  TODO: nothing? function => R? Mem => R?
//  TODO: rename to op_tangent_type
const Def* tangent_type_fun(const Def* ty) { return ty; }

/// computes pb type `E* -> A*`
/// `E` - type of the expression (return type for a function)
/// `A` - type of the argument (point of orientation resp. derivative - argument type for partial pullbacks)
const Pi* pullback_type(const Def* E, const Def* A) {
    auto& world   = E->world();
    auto tang_arg = tangent_type_fun(A);
    auto tang_ret = tangent_type_fun(E);
    auto pb_ty    = world.cn({tang_ret, world.cn(tang_arg)});
    return pb_ty;
}

namespace {
// `A,R` => `(A->R)' = A' -> R' * (R* -> A*)`
const Pi* autodiff_type_fun(const Def* arg, const Def* ret) {
    auto& world = arg->world();
    world.DLOG("autodiff type for {} => {}", arg, ret);
    auto aug_arg = mim::plug::autodiff::autodiff_type_fun(arg);
    auto aug_ret = mim::plug::autodiff::autodiff_type_fun(ret);
    world.DLOG("augmented types: {} => {}", aug_arg, aug_ret);
    if (!aug_arg || !aug_ret) return nullptr;
    // `Q* -> P*`
    auto pb_ty = pullback_type(ret, arg);
    world.DLOG("pb type: {}", pb_ty);
    // `P' -> Q' * (Q* -> P*)`

    auto deriv_ty = world.cn({aug_arg, world.cn({aug_ret, pb_ty})});
    world.DLOG("autodiff type: {}", deriv_ty);
    return deriv_ty;
}
} // namespace

const Pi* autodiff_type_fun_pi(const Pi* pi) {
    auto& world = pi->world();
    if (!Pi::isa_cn(pi)) {
        // TODO: dependency
        auto arg = pi->dom();
        auto ret = pi->codom();
        if (ret->isa<Pi>()) {
            auto aug_arg = autodiff_type_fun(arg);
            if (!aug_arg) return nullptr;
            auto aug_ret = autodiff_type_fun(pi->codom());
            if (!aug_ret) return nullptr;
            return world.pi(aug_arg, aug_ret);
        }
        return autodiff_type_fun(arg, ret);
    }
    auto [arg, ret_pi] = pi->doms<2>();
    auto ret           = ret_pi->as<Pi>()->dom();
    world.DLOG("compute AD type for pi");
    return autodiff_type_fun(arg, ret);
}

// In general transforms `A` => `A'`.
// Especially `P->Q` => `P'->Q' * (Q* -> P*)`.
const Def* autodiff_type_fun(const Def* ty) {
    auto& world = ty->world();
    // TODO: handle DS (operators)
    if (auto pi = ty->isa<Pi>()) return autodiff_type_fun_pi(pi);
    // Also handles autodiff call from axm declaration => abstract => leave it.
    world.DLOG("AutoDiff on type: {} <{}>", ty, ty->node_name());
    if (Idx::isa(ty)) return ty;
    if (ty == world.type_nat()) return ty;
    if (auto arr = ty->isa<Arr>()) {
        auto shape   = arr->arity();
        auto body    = arr->body();
        auto body_ad = autodiff_type_fun(body);
        if (!body_ad) return nullptr;
        return world.arr(shape, body_ad);
    }
    if (auto sig = ty->isa<Sigma>()) {
        // TODO: mut sigma
        auto ops = DefVec(sig->ops(), [&](const Def* op) { return autodiff_type_fun(op); });
        world.DLOG("ops: {}", fe::Join(ops));
        return world.sigma(ops);
    }
    // mem
    if (Axm::isa<mem::M>(ty)) return ty;
    world.WLOG("no-diff type: {}", ty);
    return nullptr;
}

const Def* zero_def(const Def* T) {
    // TODO: we want: zero mem -> zero mem or bot
    // zero [A,B,C] -> [zero A, zero B, zero C]
    auto& world = T->world();
    world.DLOG("zero_def for type {} <{}>", T, T->node_name());
    if (auto arr = T->isa<Arr>()) {
        auto arity      = arr->arity();
        auto body       = arr->body();
        auto inner_zero = world.app(world.annex<zero>(), body);
        auto zero_arr   = world.pack(arity, inner_zero);
        world.DLOG("zero_def for array of shape {} with type {}", arity, body);
        world.DLOG("zero_arr: {}", zero_arr);
        return zero_arr;
    } else if (Idx::isa(T)) {
        // TODO: real
        auto zero = world.lit(T, 0)->set("zero");
        world.DLOG("zero_def for int is {}", zero);
        return zero;
    } else if (auto sig = T->isa<Sigma>()) {
        auto ops = DefVec(sig->ops(), [&](const Def* op) { return world.app(world.annex<zero>(), op); });
        return world.tuple(ops);
    }

    // or return bot
    // or id => zero T
    // return world.app(world.annex<zero>(), T);
    return nullptr;
}

const Def* op_sum(const Def* T, Defs defs) {
    // TODO: assert all are of type T
    auto& world = T->world();
    return world.app(world.app(world.annex<sum>(), {world.lit_nat(defs.size()), T}), defs);
}

} // namespace mim::plug::autodiff
