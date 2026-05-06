#include <mim/plug/direct/direct.h>

#include "mim/plug/autodiff/autodiff.h"
#include "mim/plug/autodiff/pass/eval.h"

using namespace std::literals;

namespace mim::plug::autodiff {

const Def* Eval::augment_lit(const Lit* lit, Lam* f, Lam*) {
    auto pb               = zero_pullback(lit->type(), f->dom(2, 0));
    partial_pullback[lit] = pb;
    return lit;
}

const Def* Eval::augment_var(const Var* var, Lam*, Lam*) {
    assert(augmented.count(var));
    auto aug_var = augmented[var];
    assert(partial_pullback.count(aug_var));
    return var;
}

const Def* Eval::augment_lam(Lam* lam, Lam* f, Lam* f_diff) {
    // TODO: we need partial pullbacks for tuples (higher-order / ret-cont application)
    // also for higher-order args, ret_cont (at another point)
    // the pullback is not important but formally required by tuple rule
    if (augmented.count(lam)) {
        // We already know the function:
        // * recursion
        // * higher order arguments
        // * new encounter of previous function
        DLOG("already augmented {} : {} to {} : {}", lam, lam->type(), augmented[lam], augmented[lam]->type());
        return augmented[lam];
    }
    // TODO: better fix (another pass as analysis?)
    // TODO: handle open functions
    if (Lam::isa_basicblock(lam) || lam->sym().view().contains("ret") || lam->sym().view().contains("_cont")) {
        // A open continuation behaves the same as return:
        // ```
        // cont: Cn[X]
        // cont': Cn[X,Cn[X,A]]
        // ```
        // There is dependency on the closed function context.
        // (All derivatives are with respect to the arguments of a closed function.)

        DLOG("found an open continuation {} : {}", lam, lam->type());
        auto cont_dom = lam->type()->dom(); // not only 0 but all
        auto pb_ty    = pullback_type(cont_dom, f->dom(2, 0));
        auto aug_dom  = autodiff_type_fun(cont_dom);
        DLOG("augmented domain {}", aug_dom);
        DLOG("pb type is {}", pb_ty);
        auto aug_lam              = world().mut_con({aug_dom, pb_ty})->set("aug_"s + lam->sym().str());
        auto aug_var              = aug_lam->var((nat_t)0);
        augmented[lam->var()]     = aug_var;
        augmented[lam]            = aug_lam; // TODO: only one of these two
        derived[lam]              = aug_lam;
        auto pb                   = aug_lam->var(1);
        partial_pullback[aug_var] = pb;
        // We are still in same closed function.
        auto new_body = augment(lam->body(), f, f_diff);
        // TODO we also need to rewrite the filter
        aug_lam->set(lam->filter(), new_body);

        auto lam_pb               = zero_pullback(lam->type(), f->dom(2, 0));
        partial_pullback[aug_lam] = lam_pb;
        DLOG("augmented {} : {}", lam, lam->type());
        DLOG("to {} : {}", aug_lam, aug_lam->type());
        DLOG("ppb for lam cont: {}", lam_pb);

        return aug_lam;
    }
    DLOG("found a closed function call {} : {}", lam, lam->type());
    // Some general function in the program needs to be differentiated.
    auto aug_lam = world().call<ad>(lam);
    // TODO: directly more association here? => partly inline op_autodiff
    DLOG("augmented function is {} : {}", aug_lam, aug_lam->type());
    return aug_lam;
}

const Def* Eval::augment_extract(const Extract* ext, Lam* f, Lam* f_diff) {
    auto tuple = ext->tuple();
    auto index = ext->index();

    auto aug_tuple = augment(tuple, f, f_diff);
    auto aug_index = augment(index, f, f_diff);

    const Def* pb;
    DLOG("tuple was: {} : {}", tuple, tuple->type());
    DLOG("aug tuple: {} : {}", aug_tuple, aug_tuple->type());
    if (shadow_pullback.count(aug_tuple)) {
        auto shadow_tuple_pb = shadow_pullback[aug_tuple];
        DLOG("Shadow pullback: {} : {}", shadow_tuple_pb, shadow_tuple_pb->type());
        pb = world().extract(shadow_tuple_pb, aug_index);
    } else {
        // ```
        // e:T, b:B
        // b = e#i
        // b* = \lambda (s:B). e* (insert s at i in (zero T))
        // ```
        assert(partial_pullback.count(aug_tuple));
        auto tuple_pb = partial_pullback[aug_tuple];
        auto pb_ty    = pullback_type(ext->type(), f->dom(2, 0));
        auto pb_fun   = world().mut_lam(pb_ty)->set("extract_pb");
        DLOG("Pullback: {} : {}", pb_fun, pb_fun->type());
        auto pb_tangent = pb_fun->var(0uz)->set("s");
        auto tuple_tan  = world().insert(world().call<zero>(aug_tuple->type()), aug_index, pb_tangent)->set("tup_s");
        pb_fun->app(true, tuple_pb, {tuple_tan, pb_fun->var(1) /* ret_var but make sure to select correct one */});
        pb = pb_fun;
    }

    auto aug_ext              = world().extract(aug_tuple, aug_index);
    partial_pullback[aug_ext] = pb;

    return aug_ext;
}

const Def* Eval::augment_tuple(const Tuple* tup, Lam* f, Lam* f_diff) {
    // TODO: should use ops instead?
    auto aug_ops = tup->projs([&](const Def* op) -> const Def* { return augment(op, f, f_diff); });
    auto aug_tup = world().tuple(aug_ops);

    auto pbs = DefVec(Defs(aug_ops), [&](const Def* op) { return partial_pullback[op]; });
    DLOG("tuple pbs {,}", pbs);
    // shadow pb = tuple of pbs
    auto shadow_pb           = world().tuple(pbs);
    shadow_pullback[aug_tup] = shadow_pb;

    // ```
    // \lambda (s:[E0,...,Em]).
    //    sum (m,A)
    //      ((cps2ds e0*) (s#0), ..., (cps2ds em*) (s#m))
    // ```
    auto pb_ty = pullback_type(tup->type(), f->dom(2, 0));
    auto pb    = world().mut_lam(pb_ty)->set("tup_pb");
    DLOG("Augmented tuple: {} : {}", aug_tup, aug_tup->type());
    DLOG("Tuple Pullback: {} : {}", pb, pb->type());
    DLOG("shadow pb: {} : {}", shadow_pb, shadow_pb->type());

    auto pb_tangent = pb->var(0uz)->set("tup_s");

    auto tangents = DefVec(pbs.size(), [&](nat_t i) {
        return world().app(direct::op_cps2ds_dep(pbs[i]), world().extract(pb_tangent, i));
    });
    pb->app(true, pb->var(1),
            // summed up tangents
            op_sum(tangent_type_fun(f->dom(2, 0)), tangents));
    partial_pullback[aug_tup] = pb;

    return aug_tup;
}

const Def* Eval::augment_pack(const Pack* pack, Lam* f, Lam* f_diff) {
    auto arity = pack->arity(); // TODO: arity vs shape
    auto body  = pack->body();

    auto aug_arity = augment_(arity, f, f_diff);
    auto aug_body  = augment(body, f, f_diff);

    auto aug_pack = world().pack(aug_arity, aug_body);

    assert(partial_pullback[aug_body] && "pack pullback should exists");
    // TODO: or use scale axm
    auto body_pb              = partial_pullback[aug_body];
    auto pb_pack              = world().pack(aug_arity, body_pb);
    shadow_pullback[aug_pack] = pb_pack;

    DLOG("shadow pb of pack: {} : {}", pb_pack, pb_pack->type());

    auto pb_type = pullback_type(pack->type(), f->dom(2, 0));
    auto pb      = world().mut_lam(pb_type)->set("pack_pb");

    DLOG("pb of pack: {} : {}", pb, pb_type);

    auto f_arg_ty_diff = tangent_type_fun(f->dom(2, 0));
    auto app_pb        = world().mut_pack(world().arr(aug_arity, f_arg_ty_diff));

    // TODO: special case for const width (special tuple)

    // <i:n, cps2ds body_pb (s#i)>
    app_pb->set(world().app(direct::op_cps2ds_dep(body_pb), world().extract(pb->var((nat_t)0), app_pb->var())));

    DLOG("app pb of pack: {} : {}", app_pb, app_pb->type());

    auto sumup = world().app(world().annex<sum>(), {aug_arity, f_arg_ty_diff});
    DLOG("sumup: {} : {}", sumup, sumup->type());

    pb->app(true, pb->var(1), world().app(sumup, app_pb));

    partial_pullback[aug_pack] = pb;

    return aug_pack;
}

const Def* Eval::augment_app(const App* app, Lam* f, Lam* f_diff) {
    auto callee = app->callee();
    auto arg    = app->arg();

    auto aug_arg    = augment(arg, f, f_diff);
    auto aug_callee = augment(callee, f, f_diff);

    DLOG("augmented argument <{}> {} : {}", aug_arg->unique_name(), aug_arg, aug_arg->type());
    DLOG("augmented callee  <{}> {} : {}", aug_callee->unique_name(), aug_callee, aug_callee->type());
    // TODO: move down to if(!is_cont(callee))
    if (!Pi::isa_cn(callee->type()) && Pi::isa_cn(aug_callee->type())) {
        aug_callee = direct::op_cps2ds_dep(aug_callee);
        DLOG("wrapped augmented callee: <{}> {} : {}", aug_callee->unique_name(), aug_callee, aug_callee->type());
    }

    // nested (inner application)
    if (app->type()->isa<Pi>()) {
        DLOG("Nested application callee: {} : {}", aug_callee, aug_callee->type());
        DLOG("Nested application arg: {} : {}", aug_arg, aug_arg->type());
        auto aug_app = world().app(aug_callee, aug_arg);
        DLOG("Nested application result: <{}> {} : {}", aug_app->unique_name(), aug_app, aug_app->type());
        // We do not add a pullback as the pullback is bundled in the cps call or returned by the ds call
        return aug_app;
    }

    // continuation (ret, if, ...)
    if (Pi::isa_basicblock(callee->type())) {
        // TODO: check if function (not operator)
        // The original function is an open function (return cont / continuation) of type `Cn[E]`
        // The augmented function `aug_callee` looks like a function but is not really a function has the type `Cn[E,
        // Cn[E, Cn[A]]]`

        // ret(e) => ret'(e, e*)

        DLOG("continuation {} : {} => {} : {}", callee, callee->type(), aug_callee, aug_callee->type());

        auto arg_pb  = partial_pullback[aug_arg];
        auto aug_app = world().app(aug_callee, {aug_arg, arg_pb});
        DLOG("Augmented application: {} : {}", aug_app, aug_app->type());
        return aug_app;
    }

    // ds function
    if (!Pi::isa_cn(callee->type())) {
        auto aug_app = world().app(aug_callee, aug_arg);
        DLOG("Augmented application: <{}> {} : {}", aug_app->unique_name(), aug_app, aug_app->type());

        DLOG("ds function: {} : {}", aug_app, aug_app->type());
        // The calle is ds function (e.g. operator (or its partial application))
        auto [aug_res, fun_pb] = aug_app->projs<2>();
        // We compose `fun_pb` with `argument_pb` to get the result pb
        // TODO: combine case with cps function case
        auto arg_pb = partial_pullback[aug_arg];
        assert(arg_pb);
        // `fun_pb: out_tan -> arg_tan`
        // `arg_pb: arg_tan -> fun_tan`
        DLOG("function pullback: {} : {}", fun_pb, fun_pb->type());
        DLOG("argument pullback: {} : {}", arg_pb, arg_pb->type());
        auto res_pb = compose_cn(arg_pb, fun_pb);
        DLOG("result pullback: {} : {}", res_pb, res_pb->type());
        partial_pullback[aug_res] = res_pb;
        world().debug_dump();
        return aug_res;
    }

    // TODO: dest with a function such that f args != g args
    {
        // normal function app
        // ```
        // g: cn[E, cn X]
        // g(args,cont)
        // g': cn[E, cn[X, cn[X, cn E]]]
        // g'(aug_args, ____)
        // ```
        auto g = callee;
        // At this point g_deriv might still be "autodiff ... g".
        auto g_deriv = aug_callee;
        DLOG("g: {} : {}", g, g->type());
        DLOG("g': {} : {}", g_deriv, g_deriv->type());

        auto [real_aug_args, aug_cont] = aug_arg->projs<2>();
        DLOG("real_aug_args: {} : {}", real_aug_args, real_aug_args->type());
        DLOG("aug_cont: {} : {}", aug_cont, aug_cont->type());
        auto e_pb = partial_pullback[real_aug_args];
        DLOG("e_pb (arg_pb): {} : {}", e_pb, e_pb->type());

        // TODO: better debug names
        auto ret_g_deriv_ty = g_deriv->type()->as<Pi>()->dom(1);
        DLOG("ret_g_deriv_ty: {} ", ret_g_deriv_ty);
        auto c1_ty = ret_g_deriv_ty->as<Pi>();
        DLOG("c1_ty: (cn[X, cn[X+, cn E+]]) {}", c1_ty);
        auto c1   = world().mut_lam(c1_ty)->set("c1");
        auto res  = c1->var((nat_t)0);
        auto r_pb = c1->var(1);
        c1->app(true, aug_cont, {res, compose_cn(e_pb, r_pb)});

        auto aug_app = world().app(aug_callee, {real_aug_args, c1});
        DLOG("aug_app: {} : {}", aug_app, aug_app->type());

        // The result is * => no pb needed, no composition needed.
        return aug_app;
    }
    assert(false && "should not be reached");
}

/// Rewrites the given definition in a lambda environment.
const Def* Eval::augment_(const Def* def, Lam* f, Lam* f_diff) {
    // We use macros above to avoid recomputation.
    // TODO: Alternative:
    // Use class instances to rewrite inside a function and save such values (f, f_diff, f->dom(2, 0)).

    DLOG("Augment def {} : {}", def, def->type());

    // Applications are continuations, operators, or full functions
    if (auto app = def->isa<App>()) {
        auto callee = app->callee();
        auto arg    = app->arg();
        DLOG("Augment application: app {} with {}", callee, arg);
        return augment_app(app, f, f_diff);
    } else if (auto ext = def->isa<Extract>()) {
        auto tuple = ext->tuple();
        auto index = ext->index();
        DLOG("Augment extract: {} #[{}]", tuple, index);
        return augment_extract(ext, f, f_diff);
    } else if (auto var = def->isa<Var>()) {
        DLOG("Augment variable: {}", var);
        return augment_var(var, f, f_diff);
    } else if (auto lam = def->isa_mut<Lam>()) {
        DLOG("Augment mut lambda: {}", lam);
        return augment_lam(lam, f, f_diff);
    } else if (auto lam = def->isa<Lam>()) {
        ELOG("Augment lambda: {}", lam);
        assert(false && "can not handle non-mutable lambdas");
    } else if (auto lit = def->isa<Lit>()) {
        DLOG("Augment literal: {}", def);
        return augment_lit(lit, f, f_diff);
    } else if (auto tup = def->isa<Tuple>()) {
        DLOG("Augment tuple: {}", def);
        return augment_tuple(tup, f, f_diff);
    } else if (auto pack = def->isa<Pack>()) {
        // TODO: handle mut packs (dependencies in the pack) (=> see paper about vectors)
        auto arity = pack->arity(); // TODO: arity vs shape
        auto body  = pack->body();
        DLOG("Augment pack: {} : {} with {}", arity, arity->type(), body);
        return augment_pack(pack, f, f_diff);
    } else if (auto ax = def->isa<Axm>()) {
        //  TODO: move concrete handling to own function / file / directory (file per plugin)
        DLOG("Augment axm: {} : {}", ax, ax->type());
        DLOG("axm curry: {}", ax->curry());
        DLOG("axm flags: {}", ax->flags());
        auto diff_name = ax->sym().str();
        find_and_replace(diff_name, ".", "_");
        find_and_replace(diff_name, "%", "");
        diff_name = "internal_diff_" + diff_name;
        DLOG("axm name: {}", ax->sym());
        DLOG("axm function name: {}", diff_name);

        auto diff_fun = world().externals()[world().sym(diff_name)];
        if (!diff_fun) {
            ELOG("derivation not found: {}", diff_name);
            auto expected_type = autodiff_type_fun(ax->type());
            ELOG("expected: {} : {}", diff_name, expected_type);
            assert(false && "unhandled axm");
        }
        // TODO: why does this cause a depth error?
        return diff_fun;
    }

    // TODO: handle Pi for axm app
    // TODO: remaining (lambda, axm)

    ELOG("did not expect to augment: {} : {}", def, def->type());
    ELOG("node: {}", def->node_name());
    assert(false && "augment not implemented on this def");
    fe::unreachable();
}

} // namespace mim::plug::autodiff
