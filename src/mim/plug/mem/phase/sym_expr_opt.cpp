#include "mim/plug/mem/phase/sym_expr_opt.h"

#include <absl/container/fixed_array.h>

#include "mim/def.h"

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

enum {
    Proxy_GVN,
    Proxy_Slot,
};

const Def* isa_gvn_proxy(const Def* def) {
    if (auto mem = def->isa<Proxy>())
        if (mem->tag() == Proxy_GVN) return mem;
    return nullptr;
}

const Def* isa_slot_proxy(const Def* def) {
    if (auto mem = def->isa<Proxy>())
        if (mem->tag() == Proxy_Slot) return mem;
    return nullptr;
}

Def* SymExprOpt::Analysis::rewrite_mut(Def* mut) {
    mut_stack_.push_back(mut);
    // DLOG("entering {}", mut);
    map(mut, mut);

    if (auto var = mut->has_var()) {
        map(var, var);

        if (mut->isa<Lam>())
            for (auto var : mut->tvars()) {
                map(var, var);
                lattice_[var] = var;
            }
    }

    for (auto d : mut->deps())
        rewrite(d);

    mut_stack_.pop_back();
    // DLOG("leaving {}", mut);
    return mut;
}

const Def* SymExprOpt::Analysis::propagate(const Def* var, const Def* def) {
    auto [i, ins] = lattice_.emplace(var, def);
    if (ins) {
        todo_ = true;
        DLOG("propagate: {} → {}", var, def);
        return def;
    }

    auto cur = i->second;
    if (!cur || def->isa<Bot>() || cur == def || cur == var || isa_gvn_proxy(cur)) return cur;

    todo_ = true;
    DLOG("cannot propagate {}, trying GVN", var);
    if (cur->isa<Bot>()) return i->second = def;
    return i->second = nullptr; // we reached top for propagate; nullptr marks this to bundle for GVN
}

static nat_t get_index(const Def* def) { return Lit::as(def->as<Extract>()->index()); }

const Def* SymExprOpt::Analysis::rewrite_imm_App(const App* app) {
    DLOG("imm_app of {}, currently in {}", app->callee(), mut_stack_.empty() ? nullptr : mut_stack_.back());
    if (auto store = Axm::isa<mem::store>(app)) {
        auto [mem, ptr, val]                               = store->args<3>();
        auto abstr_mem                                     = rewrite(mem);
        auto abstr_ptr                                     = rewrite(ptr);
        auto abstr_val                                     = rewrite(val);
        current_slot_values_[mut_stack_.back()][abstr_ptr] = abstr_val;
        DLOG("in {}, found a store: {} <- {}", mut_stack_.back(), ptr, val);
        return store; // TODO: not sure if we don't need to rewrite something here
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [mem, ptr] = load->args<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_ptr  = rewrite(ptr);
        DLOG("in {}, found a load from {}", mut_stack_.back(), abstr_ptr);
        if (auto known_value = current_slot_values_[mut_stack_.back()][abstr_ptr]) {
            DLOG("we know that it's {}", known_value);
            return load; // TODO: not sure if we don't need to rewrite something here
        }
    } else if (auto slot = Axm::isa<mem::slot>(app)) {
        auto [Ta, mi]                      = slot->uncurry_args<2>();
        auto [pointee_type, address_space] = Ta->projs<2>();
        auto [mem, id]                     = mi->projs<2>();
        auto [_, ptr]                      = slot->projs<2>();
        auto abstr_mem                     = rewrite(mem);
        auto abstr_id                      = rewrite(id);
        all_slots_[ptr]                    = pointee_type;
        DLOG("in {}, found declaration for slot {}", mut_stack_.back(), ptr);
        return slot; // TODO: not sure if we don't need to rewrite something here
    } else if (auto branch = Branch(app)) {
        auto abstr = branch.cond(); // TODO: probably need to do something with this?
        auto l     = Lit::isa<bool>(abstr);

        if (!l || *l)
            if (auto lam = branch.tt()->isa_mut<Lam>(); isa_optimizable(lam)) {
                DLOG("branch, writing local values to {}", lam);
                for (auto [slot, value] : current_slot_values_[mut_stack_.back()]) {
                    DLOG("{}", slot);
                    current_slot_values_[lam][slot] = value;
                }
            }
        if (!l || !*l)
            if (auto lam = branch.ff()->isa_mut<Lam>(); isa_optimizable(lam)) {
                DLOG("branch, writing local values to {}", lam);
                for (auto [slot, value] : current_slot_values_[mut_stack_.back()]) {
                    DLOG("{}", slot);
                    current_slot_values_[lam][slot] = value;
                }
            }
    } else if (auto lam = app->callee()->isa_mut<Lam>(); !isa_optimizable(lam)) {
        DLOG("{} not optimizable", app->callee());

        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);
        for (size_t i = 0; i != n; ++i)
            abstr_args[i] = rewrite(app->targ(i));

        DLOG("done analyzing args of {}", lam);

        bool mem_passed = false;
        for (auto arg : abstr_args) {
            DLOG("arg: {}", arg);
            if (Axm::isa<mem::M>(arg->type())) mem_passed = true;
        }
        if (mem_passed) {
            DLOG("a mem is passed");
            for (auto arg : abstr_args) {
                if (auto continuation = arg->isa_mut<Lam>(); isa_optimizable(continuation)) {
                    // The unknown function may call this as a continuation. In that case, the slot
                    // values are the same as for the current function.
                    for (auto [slot, current_value] : current_slot_values_[mut_stack_.back()])
                        current_slot_values_[continuation][slot] = current_value;

                    // Except for those slots that are also passed to the unknown function. We don't
                    // know what it does to those, so we set them to top.
                    for (auto arg : abstr_args) {
                        if (auto i = current_slot_values_[mut_stack_.back()].find(arg);
                            i != current_slot_values_[mut_stack_.back()].end()) {
                            DLOG("{} passed as continuation, and {} escapes, setting to top", continuation, arg);
                            current_slot_values_[continuation][arg] = arg;
                        }
                    }

                    // TODO: do we need to set todo_ or call rewrite again or something?
                }
            }
        }
    } else if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);
        auto abstr_vars = absl::FixedArray<const Def*>(n);

        // propagate
        for (size_t i = 0; i != n; ++i) {
            auto abstr    = rewrite(app->targ(i));
            abstr_vars[i] = propagate(lam->tvar(i), abstr);
            abstr_args[i] = abstr;
        }

        DLOG("propagating slot values for call of {}", lam);
        DLOG("existing slots: ");
        for (auto [slot, slot_type] : all_slots_)
            DLOG("{}", slot);

        for (auto [slot, slot_type] : all_slots_) {
            if (auto local_value = current_slot_values_[mut_stack_.back()][slot]) {
                DLOG("propagating local value: {} -> {}", slot, local_value);
                auto proxy = world().proxy(slot_type, {lam, slot}, 0, Proxy_Slot);
                propagate(proxy, local_value);
            } else {
                DLOG("propagating value from parameters");
                auto lam_proxy    = world().proxy(slot_type, {lam, slot}, 0, Proxy_Slot);
                auto lookup_proxy = world().proxy(slot_type, {mut_stack_.back(), slot}, 0, Proxy_Slot);
                if (auto i = lattice_.find(lookup_proxy); i != lattice_.end()) {
                    propagate(lam_proxy, i->second);
                } else {
                    DLOG("no value found for {}", slot);
                    // TODO find the value
                    propagate(lam_proxy, lam_proxy);
                }
            }
        }

        // GVN bundle: All things marked as top (nullptr) by propagate are now treated as one entity by bundling them
        // into one proxy
        for (size_t i = 0; i != n; ++i) {
            if (abstr_vars[i]) continue;

            auto vars = DefVec();
            auto vi   = lam->tvar(i);
            auto ai   = abstr_args[i];
            vars.emplace_back(vi);

            for (size_t j = i + 1; j != n; ++j) {
                auto vj = lam->tvar(j);
                if (!abstr_vars[j] && abstr_args[j] == ai) vars.emplace_back(vj);
            }

            if (vars.size() == 1) {
                lattice_[vi] = abstr_vars[i] = vi; // top
            } else {
                auto proxy = world().proxy(vi->type(), vars, 0, 0);

                for (auto p : proxy->ops()) {
                    auto j       = get_index(p);
                    auto vj      = lam->tvar(j);
                    lattice_[vj] = abstr_vars[j] = proxy;
                }

                DLOG("bundle: {}", proxy);
            }
        }

        // GVN split: We have to prove that all incoming args for all vars in a bundle are the same value.
        // Otherwise we have to refine the bundle by splitting off contradictions.
        // E.g.: Say we started with `{a, b, c, d, e}` as a single bundle for all tvars of `lam`.
        // Now, we see `lam (x, y, x, y, z)`. Then we have to build:
        // a -> {a, c}
        // b -> {b, d}
        // c -> {a, c}
        // d -> {b, d}
        // e -> e      (top)
        for (size_t i = 0; i != n; ++i) {
            if (auto proxy = isa_gvn_proxy(abstr_vars[i])) {
                auto num  = proxy->num_ops();
                auto vars = DefVec();
                auto ai   = abstr_args[i];

                for (auto p : proxy->ops()) {
                    auto j  = get_index(p);
                    auto vj = lam->tvar(j);
                    if (p == vj) {
                        if (ai == abstr_args[j]) vars.emplace_back(vj);
                    }
                }

                auto new_num = vars.size();
                if (new_num == 1) {
                    todo_        = true;
                    auto vi      = lam->tvar(i);
                    lattice_[vi] = abstr_vars[i] = vi;
                    DLOG("single: {}", vi);
                } else if (new_num != num) {
                    todo_          = true;
                    auto new_proxy = world().proxy(ai->type(), vars, 0, 0);
                    DLOG("split: {}", new_proxy);

                    for (auto p : new_proxy->ops()) {
                        auto j  = get_index(p);
                        auto vj = lam->tvar(j);
                        if (p == vj) lattice_[vj] = abstr_vars[j] = new_proxy;
                    }
                }
                // if new_num == num: do nothing
            }
        }

        // set new abstract var
        auto abstr_var = world().tuple(abstr_vars);
        map(lam->var(), abstr_var);
        lattice_[lam->var()] = abstr_var;

        if (!lookup(lam)) {
            // DLOG("entering {}", lam);
            mut_stack_.push_back(lam);
            for (auto d : lam->deps())
                rewrite(d);
            // DLOG("leaving {}", lam);
            mut_stack_.pop_back();
        }

        return world().app(lam, abstr_args);
    }

    return mim::Analysis::rewrite_imm_App(app);
}

static bool keep(const Def* old_var, const Def* abstr) {
    if (old_var == abstr) return true; // top
    auto proxy = isa_gvn_proxy(abstr);
    return proxy && proxy->op(0) == old_var; // first in GVN bundle?
}

const Def* SymExprOpt::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>()) {
        if (auto l = lattice(old_lam->var()); l && l != old_lam->var()) {
            todo_ = true;

            size_t num_old = old_lam->num_tvars();
            Lam* new_lam;
            if (auto i = lam2lam_.find(old_lam); i != lam2lam_.end())
                new_lam = i->second;
            else {
                // build new dom
                auto new_doms = DefVec();
                for (size_t i = 0; i != num_old; ++i) {
                    auto old_var = old_lam->var(num_old, i);
                    auto abstr   = lattice(old_var);
                    if (keep(old_var, abstr)) new_doms.emplace_back(rewrite(old_lam->dom(num_old, i)));
                }

                // build new lam
                size_t num_new    = new_doms.size();
                auto new_vars     = absl::FixedArray<const Def*>(num_old);
                new_lam           = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
                lam2lam_[old_lam] = new_lam;

                // build new var
                for (size_t i = 0, j = 0; i != num_old; ++i) {
                    auto old_var = old_lam->var(num_old, i);
                    auto abstr   = lattice(old_var);

                    if (keep(old_var, abstr)) {
                        auto v      = new_lam->var(num_new, j++);
                        new_vars[i] = v;
                        if (abstr != old_var) map(abstr, v); // GVN bundle
                    } else {
                        new_vars[i] = rewrite(abstr); // SCCP propagate
                    }
                }

                map(old_lam->var(), new_vars);
                new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
            }

            // build new app
            size_t num_new = new_lam->num_vars();
            auto new_args  = absl::FixedArray<const Def*>(num_new);
            for (size_t i = 0, j = 0; i != num_old; ++i) {
                auto old_var = old_lam->var(num_old, i);
                auto abstr   = lattice(old_var);
                if (keep(old_var, abstr)) new_args[j++] = rewrite(old_app->targ(i));
            }

            return map(old_app, new_world().app(new_lam, new_args));
        }
    }

    return Rewriter::rewrite_imm_App(old_app);
}

} // namespace mim::plug::mem::phase
