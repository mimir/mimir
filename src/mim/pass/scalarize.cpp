#include "mim/pass/scalarize.h"

#include "mim/tuple.h"

#include "mim/pass/eta_exp.h"

namespace mim {

void Scalarize::init(PassMan* man) {
    Pass::init(man);
    eta_exp_ = man->find<EtaExp>();
}

// TODO should also work for mutable non-dependent sigmas

// TODO merge with make_scalar
bool Scalarize::should_expand(Lam* lam) {
    if (!isa_optimizable(lam)) return false;
    if (auto i = tup2sca_.find(lam); i != tup2sca_.end() && i->second && i->second == lam) return false;

    auto pi = lam->type();
    if (lam->num_doms() > 1 && Pi::isa_cn(pi) && pi->isa_imm()) return true; // no ugly dependent pis

    tup2sca_[lam] = lam;
    return false;
}

Lam* Scalarize::make_scalar(const Def* def) {
    auto tup_lam = def->isa_mut<Lam>();
    assert(tup_lam);
    if (auto i = tup2sca_.find(tup_lam); i != tup2sca_.end()) return i->second;

    auto types  = DefVec();
    auto arg_sz = Vector<size_t>();
    bool todo   = false;
    for (size_t i = 0, e = tup_lam->num_doms(); i != e; ++i) {
        auto n = flatten(types, tup_lam->dom(i), false);
        arg_sz.push_back(n);
        todo |= n != 1 || types.back() != tup_lam->dom(i);
    }

    if (!todo) return tup2sca_[tup_lam] = tup_lam;

    auto cn      = world().cn(types);
    auto sca_lam = tup_lam->stub(cn);
    if (eta_exp_) eta_exp_->new2old(sca_lam, tup_lam);
    size_t n = 0;
    DLOG("type {} ~> {}", tup_lam->type(), cn);
    auto new_vars = world().tuple(DefVec(tup_lam->num_doms(), [&](auto i) {
        auto tuple = DefVec(arg_sz.at(i), [&](auto) { return sca_lam->var(n++); });
        return unflatten(tuple, tup_lam->dom(i), false);
    }));
    sca_lam->set(tup_lam->reduce(new_vars));
    tup2sca_[sca_lam] = sca_lam;
    tup2sca_.emplace(tup_lam, sca_lam);
    DLOG("lambda {} : {} ~> {} : {}", tup_lam, tup_lam->type(), sca_lam, sca_lam->type());
    return sca_lam;
}

const Def* Scalarize::rewrite(const Def* def) {
    auto& w = world();
    if (auto app = def->isa<App>()) {
        const Def* sca_callee = app->callee();

        if (auto tup_lam = sca_callee->isa_mut<Lam>(); should_expand(tup_lam)) {
            sca_callee = make_scalar(tup_lam);

        } else if (auto proj = sca_callee->isa<Extract>()) {
            auto tuple = proj->tuple()->isa<Tuple>();
            if (tuple && std::all_of(tuple->ops().begin(), tuple->ops().end(), [&](const Def* op) {
                    return should_expand(op->isa_mut<Lam>());
                })) {
                auto new_tuple = w.tuple(DefVec(tuple->num_ops(), [&](auto i) { return make_scalar(tuple->op(i)); }));
                sca_callee     = w.extract(new_tuple, proj->index());
                DLOG("Expand tuple: {} ~> {}", fe::Join(tuple->ops()), fe::Join(new_tuple->ops()));
            }
        }

        if (sca_callee != app->callee()) {
            auto new_args = DefVec();
            flatten(new_args, app->arg(), false);
            return world().app(sca_callee, new_args);
        }
    }
    return def;
}

} // namespace mim
