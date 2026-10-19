#include "mim/plug/mem/escape_analysis.h"

#include <algorithm>

#include <mim/tuple.h>

namespace mim::plug::mem {

namespace {

void dedup(DefVec& defs) {
    std::ranges::sort(defs, {}, &Def::gid);
    defs.erase(std::ranges::unique(defs).begin(), defs.end());
}

} // namespace

const Def* EscapeAnalysis::rewrite_imm(const Def* def) {
    auto res = mim::Analysis::rewrite_imm(def);
    if (def->isa<Var>() || !def->type()) return res;

    // A value whose type cannot hold a handle (a loaded scalar, say) cannot alias one.
    if (!may_hold(def->type())) {
        // Unless it is computed from one, as a bitcast would.
        if (auto app = res->isa<App>(); app && !Pi::isa_cn(app->callee()->type()) && !mem_def(app->arg()))
            escape(app->arg());
        return res;
    }

    auto carried = DefVec();
    if (auto app = res->isa<App>()) {
        if (allocates(app)) {
            carried.emplace_back(app);
        } else {
            join(carried, app->arg());
            if (!app->callee()->isa_mut<Lam>()) join(carried, app->callee());
        }
    } else if (Var::isa_tproj(res).first || res->isa_mut()) {
        join(carried, res);
    } else {
        for (auto op : res->ops())
            join(carried, op);
    }
    dedup(carried);

    if (carried.empty()) return res;
    if (res != def) aliases_[res] = carried;
    aliases_[def] = std::move(carried);
    return res;
}

const Def* EscapeAnalysis::rewrite_imm_App(const App* app) {
    if (auto alloc = freed_alloc(app)) freed_.emplace(alloc);
    return mim::Analysis::rewrite_imm_App(app);
}

void EscapeAnalysis::leave() {
    auto lam = curr_mut<Lam>();
    if (!lam || !lam->is_set()) return;

    auto tail = lam->body()->isa<App>();
    auto ops  = tail ? walk(lam, tail) : std::nullopt;
    if (!ops) {
        if (!tail) return escape(lam->body());
        escape(tail->callee());
        escape(tail->arg());
        return;
    }

    auto escaping = escaping_.contains(lam);
    auto& chain = chains_[lam] = Chain{tail, std::move(*ops), {}, {}};
    auto own                   = DefSet(chain.ops.begin(), chain.ops.end());
    for (size_t i = 0, n = chain.ops.size(); i != n; ++i) {
        auto op      = chain.ops[i];
        auto carried = DefVec();
        join(carried, op->callee());
        join(carried, op->arg());
        dedup(carried);
        for (auto d : carried) {
            if (auto k = d->isa_mut<Lam>()) {
                escape(k);
                continue;
            }
            chain.last[d] = i;
            if (escaping)
                pin(d);
            else if (!own.contains(d))
                shared_.emplace(d);
        }

        // A pointer stored into memory may be loaded again anywhere.
        if (auto store = Axm::isa<mem::store>(op)) {
            escape(store->arg(3, 2));
        } else if (!Axm::isa<mem::load>(op) && !freed_alloc(op)) {
            escape(op->callee());
            escape(op->arg());
        }
    }

    if (auto known = jumps(tail->callee()); !known.empty()) {
        for (auto k : known)
            jump(k, tail, chain.targets);
    } else {
        control(tail->callee(), chain.targets);
        for (auto arg : tail->targs())
            pass(rewrite(arg), chain.targets);
    }

    if (escaping)
        for (auto target : chain.targets)
            if (auto k = target->isa_mut<Lam>()) escape(k);
}

bool EscapeAnalysis::may_hold(const Def* type) const {
    if (type->isa_mut() || type->isa<Pi>() || Axm::isa<mem::Ptr>(type)) return true;
    if (auto sigma = type->isa<Sigma>())
        return std::ranges::any_of(sigma->ops(), [this](const Def* op) { return may_hold(op); });
    if (auto arr = type->isa<Arr>()) return may_hold(arr->body());
    return false;
}

std::optional<fe::Vector<const App*>> EscapeAnalysis::walk(Lam* lam, const App* tail) {
    auto ops  = fe::Vector<const App*>();
    auto done = LamSet();
    if (auto mem = exit_mem(tail, done); mem && !walk(lam, mem, ops)) return {};
    std::ranges::reverse(ops);
    return ops;
}

const Def* EscapeAnalysis::walk(Lam* lam, const Def* mem, fe::Vector<const App*>& ops) {
    auto var  = mem_var(lam);
    auto lvar = lam->has_var();
    while (mem != var) {
        if (!var && (!lvar || !mem->has_free_var(lvar))) break;
        auto ex = mem->isa<Extract>();
        auto op = ex ? ex->tuple()->isa<App>() : mem->isa<App>();
        if (!op || !(mem = mem_def(op->arg()))) return nullptr;
        ops.emplace_back(op);
    }
    return mem;
}

const Def* EscapeAnalysis::exit_mem(const App* tail, LamSet& done) {
    if (auto mem = mem_def(tail->arg())) return mem;

    const Def* res = nullptr;
    for (auto arg : tail->targs())
        if (auto k = arg->isa_mut<Lam>(); k && k->is_set() && !mem_var(k) && done.emplace(k).second)
            if (auto body = k->body()->isa<App>())
                if (auto mem = exit_mem(body, done)) {
                    auto ops   = fe::Vector<const App*>();
                    auto start = walk(k, mem, ops);
                    if (!start || (res && start != res)) return nullptr;
                    res = start;
                }
    return res;
}

void EscapeAnalysis::join(DefVec& carried, const Def* def) const {
    if (auto lam = def->isa_mut<Lam>()) {
        if (!lam->is_closed()) carried.emplace_back(lam);
    } else if (auto [var, i] = Var::isa_tproj(def); var && i) {
        if (auto val = lattice(def); val && val != def) join(carried, val);
    } else if (var) {
        if (auto lam = var->binder()->isa_mut<Lam>())
            for (auto tvar : lam->tvars())
                if (auto val = lattice(tvar); val && val != tvar) join(carried, val);
    } else if (auto i = aliases_.find(def); i != aliases_.end()) {
        carried.insert(carried.end(), i->second.begin(), i->second.end());
    }
}

void EscapeAnalysis::escape(const Def* def) {
    auto carried = DefVec();
    join(carried, def);
    for (auto d : carried)
        if (auto lam = d->isa_mut<Lam>())
            escape(lam);
        else
            pin(d);
}

fe::Vector<Lam*> EscapeAnalysis::jumps(const Def* callee) {
    auto known = [](const Def* def) -> Lam* {
        if (auto k = Lam::isa_rewritable(def); k && !k->is_dependent()) return k;
        return nullptr;
    };

    if (auto k = known(callee)) return {k};
    auto res = fe::Vector<Lam*>();
    if (auto sw = Switch(callee)) {
        for (size_t i = 0, n = sw.num_targets(); i != n; ++i) {
            auto k = known(sw.target(i));
            if (!k) return {};
            res.emplace_back(k);
        }
    }
    return res;
}

void EscapeAnalysis::jump(Lam* k, const App* tail, DefVec& targets) {
    targets.emplace_back(k);
    for (size_t i = 0, n = k->num_tvars(); i != n; ++i) {
        auto arg = rewrite(tail->targ(i));
        auto var = k->tvar(i);
        if (Pi::isa_cn(arg->type()))
            control(arg, targets);
        else if (may_hold(var->type()))
            propagate(k, var, arg);
    }
}

void EscapeAnalysis::propagate(Lam* k, const Def* var, const Def* val) {
    if (val == var) return;
    auto cur = lattice(var);
    if (cur == var) return escape(val);
    auto outside = val->in_scope(k) && !val->free_vars().contains(k->has_var());
    if (outside && !cur) {
        lattice(var, val);
        return;
    }
    if (outside && cur == val) return;
    escape(val);
    if (cur) escape(cur);
    pin(var);
}

void EscapeAnalysis::control(const Def* def, DefVec& targets) {
    if (def->is_closed()) return;
    if (auto tuple = def->isa<Tuple>()) {
        for (auto op : tuple->ops())
            control(op, targets);
    } else if (auto sw = Switch(def)) {
        for (size_t i = 0, n = sw.num_targets(); i != n; ++i)
            control(sw.target(i), targets);
    } else if (auto app = def->isa<App>(); app && app->axm()) {
        control(app->callee(), targets);
        pass(app->arg(), targets);
    } else {
        targets.emplace_back(def);
        if (!def->isa_mut()) escape(def);
    }
}

} // namespace mim::plug::mem
