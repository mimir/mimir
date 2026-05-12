#include "mim/plug/clos/clos.h"

#include <mim/config.h>
#include <mim/pass.h>

#include <mim/pass/eta_exp.h>
#include <mim/pass/eta_red.h>
#include <mim/pass/scalarize.h>

#include "mim/plug/clos/pass/branch_clos_elim.h"
#include "mim/plug/clos/pass/clos2sjlj.h"
#include "mim/plug/clos/pass/clos_conv_prep.h"
#include "mim/plug/clos/pass/lower_typed_clos_prep.h"
#include "mim/plug/clos/phase/clos_conv.h"
#include "mim/plug/clos/phase/lower_typed_clos.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    // clang-format off
    // phases
    Stage::hook<clos::clos_conv_phase,            clos::ClosConv          >(stages);
    Stage::hook<clos::lower_typed_clos_phase,     clos::LowerTypedClos    >(stages);
    // passes
    Stage::hook<clos::clos_conv_prep_pass,        clos::ClosConvPrep      >(stages);
    Stage::hook<clos::branch_clos_pass,           clos::BranchClosElim    >(stages);
    Stage::hook<clos::lower_typed_clos_prep_pass, clos::LowerTypedClosPrep>(stages);
    Stage::hook<clos::clos2sjlj_pass,             clos::Clos2SJLJ         >(stages);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"clos", MIM_VERSION, clos::register_normalizers, reg_stages, nullptr};
}

namespace mim::plug::clos {

/*
 * ClosLit
 */

const Def* ClosLit::env() {
    assert(def_);
    return std::get<2_u64>(clos_unpack(def_));
}

const Def* ClosLit::fnc() {
    assert(def_);
    return std::get<1_u64>(clos_unpack(def_));
}

Lam* ClosLit::fnc_as_lam() {
    auto f = fnc();
    if (auto a = Axm::isa<attr>(f)) f = a->arg();
    return f->isa_mut<Lam>();
}

const Def* ClosLit::env_var() { return fnc_as_lam()->var(Clos_Env_Param); }

ClosLit isa_clos_lit(const Def* def, bool lambda_or_branch) {
    auto tpl = def->isa<Tuple>();
    if (tpl && isa_clos_type(def->type())) {
        auto a   = attr::bottom;
        auto fnc = std::get<1_u64>(clos_unpack(tpl));
        if (auto fa = Axm::isa<attr>(fnc)) {
            fnc = fa->arg();
            a   = fa.id();
        }
        if (!lambda_or_branch || fnc->isa<Lam>()) return ClosLit(tpl, a);
    }
    return ClosLit(nullptr, attr::bottom);
}

const Def* clos_pack(const Def* env, const Def* lam, const Def* ct) {
    assert(env && lam);
    assert(!ct || isa_clos_type(ct));
    auto& w = env->world();
    auto pi = lam->type()->as<Pi>();
    assert(env->type() == pi->dom(Clos_Env_Param));
    ct = (ct) ? ct : clos_type(w.cn(clos_remove_env(pi->dom())));
    return w.tuple(ct, {env->type(), lam, env})->isa<Tuple>();
}

std::tuple<const Def*, const Def*, const Def*> clos_unpack(const Def* c) {
    assert(c && isa_clos_type(c->type()));
    // auto& w       = c->world();
    // auto env_type = c->proj(0_u64);
    // // auto pi       = clos_type_to_pi(c->type(), env_type);
    // auto fn       = w.extract(c, w.lit_idx(3, 1));
    // auto env      = w.extract(c, w.lit_idx(3, 2));
    // return {env_type, fn, env};
    auto [ty, pi, env] = c->projs<3>();
    return {ty, pi, env};
}

const Def* clos_apply(const Def* closure, const Def* args) {
    auto& w           = closure->world();
    auto [_, fn, env] = clos_unpack(closure);
    auto pi           = fn->type()->as<Pi>();
    return w.app(fn, DefVec(pi->num_doms(), [&](auto i) { return clos_insert_env(i, env, args); }));
}

/*
 * closure types
 */

const Sigma* isa_clos_type(const Def* def) {
    auto& w  = def->world();
    auto sig = def->isa_mut<Sigma>();
    if (!sig || sig->num_ops() < 3 || sig->op(0_u64) != w.type()) return nullptr;
    auto var = sig->var(0_u64);
    if (sig->op(2_u64) != var) return nullptr;
    auto pi = sig->op(1_u64)->isa<Pi>();
    return (pi && Pi::isa_cn(pi) && pi->num_ops() > 1_u64 && pi->dom(Clos_Env_Param) == var) ? sig : nullptr;
}

Sigma* clos_type(const Pi* pi) {
    auto& w   = pi->world();
    auto doms = pi->doms();
    return ctype(w, doms, nullptr)->as_mut<Sigma>();
}

const Pi* clos_type_to_pi(const Def* ct, const Def* new_env_type) {
    assert(isa_clos_type(ct));
    auto& w      = ct->world();
    auto pi      = ct->op(1_u64)->as<Pi>();
    auto new_dom = new_env_type ? clos_sub_env(pi->dom(), new_env_type) : clos_remove_env(pi->dom());
    return w.cn(new_dom);
}

/*
 * closure environments
 */

const Def* clos_insert_env(size_t i, const Def* env, std::function<const Def*(size_t)> f) {
    return (i == Clos_Env_Param) ? env : f(shift_env(i));
}

const Def* clos_remove_env(size_t i, std::function<const Def*(size_t)> f) { return f(skip_env(i)); }

const Def* ctype(World& w, Defs doms, const Def* env_type) {
    if (!env_type) {
        auto sigma = w.mut_sigma(w.type(), 3_u64)->set("Clos");
        sigma->set(0_u64, w.type());
        sigma->set(1_u64, ctype(w, doms, sigma->var(0_u64)));
        sigma->set(2_u64, sigma->var(0_u64));
        return sigma;
    }
    return w.cn(
        DefVec(doms.size() + 1, [&](auto i) { return clos_insert_env(i, env_type, [&](auto j) { return doms[j]; }); }));
}

} // namespace mim::plug::clos
