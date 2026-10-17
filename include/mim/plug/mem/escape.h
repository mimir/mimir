#pragma once

#include <algorithm>
#include <optional>

#include <mim/phase.h>
#include <mim/tuple.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem {

/// Determines which allocations stay within the scope that allocates them.
///
/// The *home* of an allocation is the Lam on whose memory chain it sits.
/// An allocation *escapes* if it flows into the data of a tail call - returned, or handed to a callee or continuation -
/// or if an escaping Lam mentions it.
/// A Lam escapes if its use is *unknown* - it is used as a value - or if an escaping Lam jumps to it.
/// A tail call *leaves* a scope if no continuation it transfers control to is nested in that scope.
/// Control is assumed to never re-enter a scope it has left; this is also the lifetime of a `mem.slot`.
/// An axiom in control position is assumed to only pass control on to the continuations it receives.
///
/// A buffer-typed parameter of a Lam takes on the value its known call sites pass, as in SCCP.
/// A value only flows into a Lam that its scope strictly encloses; otherwise it would outlive its scope and escapes.
///
/// Lattice: an escaping allocation is ⊤ - keep as is; *absent* means it stays local.
/// A parameter is ⊤ if its call sites disagree; everything they pass escapes.
/// Subclasses add their plugin's allocating ops and the ops that hand back a handle they were given.
class EscapeAnalysis : public mim::Analysis {
public:
    struct Chain {
        const App* tail;
        fe::Vector<const App*> ops; ///< The ops threading the memory from the Lam's Var to its tail, in program order.
        fe::Vector<DefVec> uses;    ///< Per op: the allocations it mentions.
        DefVec targets;             ///< The continuations the tail transfers control to.
        DefVec locals;              ///< The allocations on this chain that do not escape, in program order.
        fe::Vector<Lam*> leaves;    ///< The scopes the tail leaves.
    };

    EscapeAnalysis(World& world, std::string name)
        : mim::Analysis(world, std::move(name)) {}

    /// @name Results
    ///@{
    /// The Lam%s with an intact memory chain.
    const LamMap<Chain>& chains() const { return chains_; }
    Defs locals(Lam* lam) const {
        if (auto i = chains_.find(lam); i != chains_.end()) return i->second.locals;
        return {};
    }
    /// Is the local @p alloc only mentioned on its home's own memory chain?
    bool confined(const Def* alloc) const { return !shared_.contains(alloc); }
    ///@}

protected:
    virtual bool allocates(const App* app) const { return Axm::isa<mem::alloc>(app) || Axm::isa<mem::malloc>(app); }
    /// The handle that @p app's result hands back, if @p app merely writes through it; `nullptr` otherwise.
    virtual const Def* hands_back(const App*) const { return nullptr; }
    virtual bool is_handle(const Def* type) const { return Axm::isa<mem::Ptr>(type); }

    /// Records what each value may carry: the allocations it aliases and the nested Lam%s in it.
    const Def* rewrite_imm(const Def* def) override {
        auto res = mim::Analysis::rewrite_imm(def);
        // A value whose type cannot hold a handle (a loaded scalar, say) cannot alias one.
        if (def->isa<Var>() || !def->type() || !may_hold(def->type())) return res;

        auto carried = DefVec();
        if (auto app = res->isa<App>()) {
            if (allocates(app))
                carried.emplace_back(app);
            else if (auto handle = hands_back(app))
                join(carried, aliases_.contains(handle) ? handle : app->arg());
            else if (join(carried, app->arg()); !app->callee()->isa_mut<Lam>())
                join(carried, app->callee());
        } else if (Var::isa_tproj(res).first || res->isa_mut()) {
            join(carried, res);
        } else {
            for (auto op : res->ops())
                join(carried, op);
        }
        if (!carried.empty()) aliases_[def] = aliases_[res] = std::move(carried);
        return res;
    }

    void leave() override {
        auto lam = curr_mut<Lam>();
        if (!lam || !lam->is_set()) return;

        auto tail = lam->body()->isa<App>();
        auto ops  = tail ? walk(lam, tail) : std::nullopt;
        if (!ops) {
            if (tail) return escape(tail->callee()), escape(tail->arg());
            return escape(lam->body());
        }

        auto escaping = escaping_.contains(lam);
        auto& chain = chains_[lam] = Chain{tail, std::move(*ops), {}, {}, {}, {}};
        for (auto op : chain.ops) {
            auto& uses = chain.uses.emplace_back();
            join(uses, op->callee());
            join(uses, op->arg());
            std::erase_if(uses, [this](const Def* use) {
                auto k = use->isa_mut<Lam>();
                if (k) escape(k);
                return k;
            });
            for (auto alloc : uses)
                if (escaping)
                    pin(alloc);
                else if (!std::ranges::contains(chain.ops, alloc))
                    shared_.emplace(alloc);
        }

        if (auto known = jumps(tail->callee()); !known.empty()) {
            for (auto k : known)
                jump(k, tail, chain.targets);
        } else {
            control(tail->callee(), chain.targets);
            if (auto tuple = tail->arg()->isa<Tuple>())
                for (auto op : tuple->ops())
                    pass(op, chain.targets);
            else
                pass(tail->arg(), chain.targets);
        }

        if (escaping)
            for (auto target : chain.targets)
                if (auto k = target->isa_mut<Lam>()) escape(k);
    }

    void finalize() override {
        for (auto& [lam, chain] : chains_) {
            chain.locals.clear();
            for (auto op : chain.ops)
                if (allocates(op) && !is_top(op)) chain.locals.emplace_back(op);
        }

        for (auto& [lam, chain] : chains_) {
            chain.leaves.clear();
            if (escaping_.contains(lam)) continue;
            auto scopes = LamSet{lam};
            collect_scopes(lam, scopes);
            for (auto scope : scopes)
                if (std::ranges::none_of(chain.targets, [scope](auto t) { return scope->nests(t); }))
                    chain.leaves.emplace_back(scope);
        }
    }

private:
    bool may_hold(const Def* type) const {
        if (type->isa_mut() || type->isa<Pi>() || is_handle(type)) return true;
        if (auto sigma = type->isa<Sigma>())
            return std::ranges::any_of(sigma->ops(), [this](const Def* op) { return may_hold(op); });
        if (auto arr = type->isa<Arr>()) return may_hold(arr->body());
        return false;
    }

    void escape(Lam* lam) {
        if (escaping_.emplace(lam).second) taint(lam), touch();
    }

    /// The Lam%s @p mut is nested in: the binders of its free Var%s, transitively.
    void collect_scopes(Def* mut, LamSet& scopes) {
        for (auto var : mut->free_vars())
            if (auto lam = var->binder()->isa_mut<Lam>(); lam && scopes.emplace(lam).second)
                collect_scopes(lam, scopes);
    }

    std::optional<fe::Vector<const App*>> walk(Lam* lam, const App* tail) {
        auto ops = fe::Vector<const App*>();
        auto cur = mem_def(tail->arg());
        if (!cur) return ops;
        auto var = mem_var(lam);
        if (!var) return {};
        while (cur != var) {
            auto ex = cur->isa<Extract>();
            auto op = ex ? ex->tuple()->isa<App>() : cur->isa<App>();
            if (!op || !(cur = mem_def(op->arg()))) return {};
            ops.emplace_back(op);
        }
        std::ranges::reverse(ops);
        return ops;
    }

    /// Adds what @p def may carry to @p carried; a nested Lam carries itself.
    void join(DefVec& carried, const Def* def) const {
        auto add = [&carried](const Def* d) {
            if (!std::ranges::contains(carried, d)) carried.emplace_back(d);
        };
        if (auto lam = def->isa_mut<Lam>()) {
            if (!lam->is_closed()) add(lam);
        } else if (auto [var, i] = Var::isa_tproj(def); var && i) {
            if (auto val = lattice(def); val && val != def) join(carried, val);
        } else if (var) {
            if (auto lam = var->binder()->isa_mut<Lam>())
                for (auto tvar : lam->tvars())
                    if (auto val = lattice(tvar); val && val != tvar) join(carried, val);
        } else if (auto i = aliases_.find(def); i != aliases_.end()) {
            for (auto d : i->second)
                add(d);
        }
    }

    /// Everything @p def may carry escapes.
    void escape(const Def* def) {
        auto carried = DefVec();
        join(carried, def);
        for (auto d : carried)
            if (auto lam = d->isa_mut<Lam>())
                escape(lam);
            else
                pin(d);
    }

    /// The Lam%s a tail jumps to directly - itself or through a jump table - or nothing.
    static fe::Vector<Lam*> jumps(const Def* callee) {
        if (auto k = callee->isa_mut<Lam>(); k && k->is_set() && !k->is_dependent()) return {k};
        auto res = fe::Vector<Lam*>();
        if (auto sw = Switch(callee)) {
            for (size_t i = 0, n = sw.num_targets(); i != n; ++i) {
                auto k = sw.target(i)->isa_mut<Lam>();
                if (!k || !k->is_set() || k->is_dependent()) return {};
                res.emplace_back(k);
            }
        }
        return res;
    }

    void jump(Lam* k, const App* tail, DefVec& targets) {
        targets.emplace_back(k);
        for (size_t i = 0, n = k->num_tvars(); i != n; ++i) {
            auto arg = tail->targ(i);
            auto var = k->tvar(i);
            if (Pi::isa_cn(arg->type()))
                control(arg, targets);
            else if (may_hold(var->type()))
                propagate(k, var, rewrite(arg));
        }
    }

    /// Joins @p val into @p k%'s parameter @p var.
    void propagate(Lam* k, const Def* var, const Def* val) {
        if (val == var) return; // a site passing var itself contributes nothing
        auto cur = lattice(var);
        if (cur == var) return escape(val);
        auto outside = std::ranges::all_of(val->free_vars(), [k](const Var* fv) {
            auto binder = fv->binder();
            return binder != k && binder->nests(k);
        });
        if (outside && !cur) return (void)lattice(var, val);
        if (outside && cur == val) return;
        escape(val);
        if (cur) escape(cur);
        pin(var);
    }

    /// A continuation handed to a tail transfers control; anything else escapes.
    void pass(const Def* def, DefVec& targets) { Pi::isa_cn(def->type()) ? control(def, targets) : escape(def); }

    /// Collects the continuations a tail transfers control to; the data along the way escapes.
    void control(const Def* def, DefVec& targets) {
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

    LamMap<Chain> chains_;
    DefMap<DefVec> aliases_; ///< What a value may carry; *absent* means nothing.
    DefSet shared_;
    LamSet escaping_;
};

} // namespace mim::plug::mem
