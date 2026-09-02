#include "mim/plug/clos/clos.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/clos/phase/branch_clos_elim.h"
#include "mim/plug/clos/phase/clos2sjlj.h"
#include "mim/plug/clos/phase/clos_conv.h"
#include "mim/plug/clos/phase/clos_conv_prep.h"
#include "mim/plug/clos/phase/lower_typed_clos.h"
#include "mim/plug/clos/phase/lower_typed_clos_prep.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    // clang-format off
    Phase::hook<clos::clos_conv_prep,        clos::phase::ClosConvPrep      >(phases);
    Phase::hook<clos::clos_conv,             clos::phase::ClosConv          >(phases);
    Phase::hook<clos::branch_clos,           clos::phase::BranchClosElim    >(phases);
    Phase::hook<clos::lower_typed_clos_prep, clos::phase::LowerTypedClosPrep>(phases);
    Phase::hook<clos::clos2sjlj,             clos::phase::Clos2SJLJ         >(phases);
    Phase::hook<clos::lower_typed_clos,      clos::phase::LowerTypedClos    >(phases);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"clos", MIM_VERSION, clos::register_normalizers, reg_phases, {}, {}};
}

namespace mim::plug::clos {

/*
 * ClosLit
 */

const Def* ClosLit::env() const { return std::get<2>(clos_unpack(def_)); }

const Def* ClosLit::fnc() const { return std::get<1>(clos_unpack(def_)); }

Lam* ClosLit::fnc_as_lam() const {
    auto f = fnc();
    if (auto a = Axm::isa<attr>(f)) f = a->arg();
    return f->isa_mut<Lam>();
}

const Def* ClosLit::env_var() const {
    auto lam = fnc_as_lam();
    return lam->var(env_param(lam->type()->as<Pi>()));
}

ClosLit isa_clos_lit(const Def* def, bool fn_isa_lam) {
    if (auto tpl = def->isa<Tuple>(); tpl && isa_clos_type(def->type())) {
        auto fnc = std::get<1>(clos_unpack(tpl));
        if (auto fa = Axm::isa<attr>(fnc)) fnc = fa->arg();
        if (!fn_isa_lam || fnc->isa<Lam>()) return ClosLit(tpl);
    }
    return ClosLit(nullptr);
}

const Def* clos_pack(const Def* env, const Def* fn, const Def* ct) {
    assert(env && fn);
    assert(!ct || isa_clos_type(ct));
    auto& w = env->world();
    auto pi = fn->type()->as<Pi>();
    auto ep = env_param(pi);
    assert(env->type() == pi->dom(ep));
    ct = ct ? ct : clos_type(w.cn(clos_remove_env(ep, pi->dom())));
    return w.tuple(ct, {env->type(), fn, env})->as<Tuple>();
}

std::tuple<const Def*, const Def*, const Def*> clos_unpack(const Def* c) {
    assert(c && isa_clos_type(c->type()));
    auto [env_type, fn, env] = c->projs<3>();
    return {env_type, fn, env};
}

const Def* clos_apply(const Def* closure, const Def* args) {
    auto& w           = closure->world();
    auto [_, fn, env] = clos_unpack(closure);
    auto pi           = fn->type()->as<Pi>();
    auto ep           = env_param(pi);
    return w.app(fn, DefVec(pi->num_doms(), [&](auto i) { return clos_insert_env(ep, i, env, args); }));
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
    if (!pi || !Pi::isa_cn(pi) || pi->num_ops() <= 1_u64) return nullptr;
    return (pi->dom(env_param(pi)) == var) ? sig : nullptr;
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
    auto ep      = env_param(pi);
    auto new_dom = new_env_type ? clos_sub_env(ep, pi->dom(), new_env_type) : clos_remove_env(ep, pi->dom());
    return w.cn(new_dom);
}

/*
 * closure environments
 */

const Def* clos_insert_env(size_t ep, size_t i, const Def* env, std::function<const Def*(size_t)> f) {
    return (i == ep) ? env : f(shift_env(ep, i));
}

const Def* clos_remove_env(size_t ep, size_t i, std::function<const Def*(size_t)> f) { return f(skip_env(ep, i)); }

const Def* ctype(World& w, Defs doms, const Def* env_type) {
    auto ep = env_param(doms);
    if (!env_type) {
        auto sigma = w.mut_sigma(w.type(), 3_u64)->set("Clos");
        sigma->set(0_u64, w.type());
        sigma->set(1_u64, ctype(w, doms, sigma->var(0_u64)));
        sigma->set(2_u64, sigma->var(0_u64));
        return sigma;
    }
    return w.cn(DefVec(doms.size() + 1,
                       [&](auto i) { return clos_insert_env(ep, i, env_type, [&](auto j) { return doms[j]; }); }));
}

} // namespace mim::plug::clos
