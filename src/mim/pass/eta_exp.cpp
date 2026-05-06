#include "mim/pass/eta_exp.h"

#include "mim/pass/eta_red.h"

using namespace std::literals;

namespace mim {

void EtaExp::init(PassMan* man) {
    Pass::init(man);
    eta_red_ = man->find<EtaRed>();
}

Lam* EtaExp::new2old(Lam* new_lam) {
    if (auto old = lookup(new2old_, new_lam)) {
        auto root = new2old(old); // path compression
        assert(root != new_lam);
        new2old_[new_lam] = root;
        return root;
    }

    return new_lam;
}

const Def* EtaExp::rewrite(const Def* def) {
    if (std::ranges::none_of(def->ops(), [](const Def* def) { return def->isa<Lam>(); })) return def;
    if (auto n = lookup(old2new(), def)) return n;

    auto [i, ins] = def2new_ops_.try_emplace(def, DefVec{});
    auto& new_ops = i->second;
    if (ins) new_ops.assign(def->ops().begin(), def->ops().end());

    for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
        if (auto lam = def->op(i)->isa_mut<Lam>(); lam && lam->is_set()) {
            if (isa_callee(def, i)) {
                if (auto orig = lookup(exp2orig_, lam)) new_ops[i] = orig;
            } else if (expand_.contains(lam)) {
                if (new_ops[i] == lam) new_ops[i] = eta_exp(lam);
            } else if (auto orig = lookup(exp2orig_, lam)) {
                if (new_ops[i] == lam) new_ops[i] = eta_exp(orig);
            }
        }
    }

    auto new_def              = def->rebuild(def->type(), new_ops);
    return old2new()[new_def] = new_def;
}

Lam* EtaExp::eta_exp(Lam* lam) {
    auto exp = lam->stub(lam->type());
    exp2orig_.emplace(exp, lam);
    exp->debug_suffix("eta_"s + lam->sym().str());
    exp->app(false, lam, exp->var());
    if (eta_red_) eta_red_->mark_irreducible(exp);
    return exp;
}

undo_t EtaExp::analyze(const Proxy* proxy) {
    DLOG("found proxy: {}", proxy);
    auto lam = proxy->op(0)->as_mut<Lam>();
    if (expand_.emplace(lam).second) return undo_visit(lam);
    return No_Undo;
}

undo_t EtaExp::analyze(const Def* def) {
    auto undo = No_Undo;
    for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
        if (auto lam = def->op(i)->isa_mut<Lam>(); lam && lam->is_set()) {
            lam = new2old(lam);
            if (expand_.contains(lam) || exp2orig_.contains(lam)) continue;

            if (isa_callee(def, i)) {
                if (auto [_, p] = *pos().emplace(lam, Pos::Callee).first; p == Pos::Non_Callee_1) {
                    DLOG("Callee: Callee -> Expand: '{}'", lam);
                    expand_.emplace(lam);
                    undo = std::min(undo, undo_visit(lam));
                } else {
                    DLOG("Callee: Bot/Callee -> Callee: '{}'", lam);
                }
            } else {
                auto [it, first] = pos().emplace(lam, Pos::Non_Callee_1);

                if (first) {
                    DLOG("Non_Callee: Bot -> Non_Callee_1: '{}'", lam);
                } else {
                    DLOG("Non_Callee: {} -> Expand: '{}'", pos2str(it->second), lam);
                    expand_.emplace(lam);
                    undo = std::min(undo, undo_visit(lam));
                }
            }
        }
    }

    return undo;
}

} // namespace mim
