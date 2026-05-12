#include "mim/plug/mem/pass/copy_prop.h"

#include <mim/pass/beta_red.h>
#include <mim/pass/eta_exp.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::pass {

void CopyProp::apply(bool bb_only) {
    bb_only_ = bb_only;
    name_ += bb_only_ ? " tt" : " ff";
}

void CopyProp::init(PassMan* man) {
    Pass::init(man);
    beta_red_ = man->find<BetaRed>();
    eta_exp_  = man->find<EtaExp>();
}

const Def* CopyProp::rewrite(const Def* def) {
    auto [app, var_lam] = isa_apped_mut_lam(def);
    if (!isa_optimizable(var_lam) || (bb_only_ && Lam::isa_returning(var_lam))) return def;

    auto n = app->num_targs();
    if (n == 0) return app;

    auto [it, _] = lam2info_.try_emplace(var_lam, std::tuple(Lattices(n), (Lam*)nullptr, DefVec(n)));
    auto& [lattice, prop_lam, old_args] = it->second;

    if (mem::mem_var(var_lam)) lattice[0] = Lattice::Keep;
    if (std::ranges::all_of(lattice, [](auto l) { return l == Lattice::Keep; })) return app;

    DefVec new_args, new_doms, appxy_ops = {var_lam};
    auto& args = data(var_lam);
    args.resize(n);

    for (size_t i = 0; i != n; ++i) {
        switch (lattice[i]) {
            case Lattice::Dead: break;
            case Lattice::Prop:
                if (app->arg(n, i)->has_dep(Dep::Proxy)) {
                    DLOG("found proxy within app: {}@{} - wait till proxy is gone", var_lam, app);
                    return app;
                } else if (args[i] == nullptr) {
                    args[i] = app->arg(n, i);
                } else if (args[i] != app->arg(n, i)) {
                    appxy_ops.emplace_back(world().lit_nat(i));
                } else {
                    assert(args[i] == app->arg(n, i));
                }
                break;
            case Lattice::Keep:
                new_doms.emplace_back(var_lam->var(n, i)->type());
                new_args.emplace_back(app->arg(n, i));
                break;
            default: std::unreachable();
        }
    }

    DLOG("app->args(): {}", fe::Join(app->args()));
    DLOG("args: {}", fe::Join(args));
    DLOG("new_args: {}", fe::Join(new_args));

    if (appxy_ops.size() > 1) {
        auto appxy = proxy(app->type(), appxy_ops, Appxy);
        DLOG("appxy: '{}': {}", appxy, fe::Join(appxy_ops));
        return appxy;
    }

    assert(new_args.size() < n);
    if (prop_lam == nullptr || !std::ranges::equal(old_args, args)) {
        old_args      = args;
        auto prop_dom = world().sigma(new_doms);
        auto new_pi   = world().pi(prop_dom, var_lam->codom());
        prop_lam      = var_lam->stub(new_pi);

        DLOG("new prop_lam: {}", prop_lam);
        if (beta_red_) beta_red_->keep(prop_lam);
        if (eta_exp_) eta_exp_->new2old(prop_lam, var_lam);

        size_t j      = 0;
        auto new_vars = DefVec(n, [&, prop_lam = prop_lam](size_t i) -> const Def* {
            switch (lattice[i]) {
                case Lattice::Dead: return proxy(var_lam->var(n, i)->type(), {var_lam, world().lit_nat(i)}, Varxy);
                case Lattice::Prop: return args[i];
                case Lattice::Keep: return prop_lam->var(j++);
                default: std::unreachable();
            }
        });

        prop_lam->set(var_lam->reduce(world().tuple(new_vars)));
    }

    DLOG("var_lam => prop_lam: {}: {} => {}: {}", var_lam, var_lam->dom(), prop_lam, prop_lam->dom());
    auto res = app->world().app(prop_lam, new_args);

    // Don't optimize again. Also, keep this line here at the very bottom as this invalidates all references.
    Lam* key = prop_lam; // prop_lam is a Lam*& which might get invalidated by the very insertion happening next.
    lam2info_.insert_or_assign(key, std::tuple(Lattices(new_doms.size(), Lattice::Keep), (Lam*)nullptr, DefVec()));
    return res;
}

undo_t CopyProp::analyze(const Proxy* proxy) {
    DLOG("found proxy: {}", proxy);
    auto var_lam                        = proxy->op(0)->as_mut<Lam>();
    auto& [lattice, prop_lam, old_args] = lam2info_[var_lam];

    if (proxy->tag() == Varxy) {
        auto i = Lit::as(proxy->op(1));
        if (auto& l = lattice[i]; l == Lattice::Dead) {
            l = Lattice::Prop;
            DLOG("Dead -> Prop: @{}#{}", var_lam, i);
            return undo_visit(var_lam);
        }
    } else {
        assert(proxy->tag() == Appxy);
        for (auto ops = proxy->ops(); auto op : ops.subspan(1)) {
            auto i = Lit::as(op);
            if (auto& l = lattice[i]; l != Lattice::Keep) {
                l = Lattice::Keep;
                DLOG("Prop -> Keep: @{}#{}", var_lam, i);
            }
        }

        return undo_visit(var_lam);
    }

    return No_Undo;
}

} // namespace mim::plug::mem::pass
