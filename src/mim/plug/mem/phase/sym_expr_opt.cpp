#include "mim/plug/mem/phase/sym_expr_opt.h"

#include <absl/container/fixed_array.h>

#include "mim/def.h"
#include "mim/phase.h"

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

enum {
    Proxy_GVN,
    Proxy_Slot,
    Proxy_Phi,
};

static const Def* isa_gvn_proxy(const Def* def) {
    if (auto mem = def->isa<Proxy>())
        if (mem->tag() == Proxy_GVN) return mem;
    return nullptr;
}

static const Def* isa_slot_proxy(const Def* def) {
    if (auto mem = def->isa<Proxy>())
        if (mem->tag() == Proxy_Slot) return mem;
    return nullptr;
}

void SymExprOpt::Analysis::reset() {
    mim::Analysis::reset();
    visited_.clear();
    mut2slot2value_.clear();
    deps_done_.clear();
}

Def* SymExprOpt::Analysis::rewrite_deps(Def* mut) {
    if (auto [_, ins] = deps_done_.emplace(mut); !ins) return mut;
    return mim::Analysis::rewrite_deps(mut);
}

void SymExprOpt::Analysis::start() {
    mim::Analysis::start();
    for (auto def : world().roots())
        analyze(def);
}

void SymExprOpt::Analysis::analyze(const Def* def) {
    if (def->isa<Var>()) return; // ignore Var's mut; muts are reached from the roots anyway
    if (auto l = lookup(def)) def = l;

    if (auto sloxy = isa_slot_proxy(def)) {
        auto slot     = sloxy2slot_[sloxy];
        auto [_, ptr] = slot->projs<2>();
        set(slot, slot);
        set(ptr, ptr);
        DLOG("setting slot to top: {}", slot);
        invalidate();
    }

    if (auto [_, ins] = visited_.emplace(def); ins)
        if (!def->isa<Proxy>())
            for (auto d : def->deps())
                analyze(d);
}

const Def* SymExprOpt::Analysis::slot2value(const Def* slot) {
    // look up the slot in the local map
    auto& slot2value = mut2slot2value_[curr_mut()];
    if (auto i = slot2value.find(slot); i != slot2value.end()) return i->second;
    // if we didn't write to the slot, and so it's not in the local map, check if we have a phi for this slot in the
    // lattice
    auto [T, as] = Axm::as<mem::Ptr>(slot->type())
                       ->args<2>(); // works because slot2value is only ever called with Ptr-typed slot proxies
    auto phi = world().proxy(T, {curr_mut(), slot}, 0, Proxy_Phi);
    if (auto it = lattice_.find(phi); it != lattice_.end()) return it->second;
    return nullptr;
}

const Def* SymExprOpt::Analysis::sccp_join(const Def* var, const Def* def) {
    DLOG("propagate called with {} and {}", var, def);
    auto [i, ins] = lattice_.emplace(var, def);
    if (ins) {
        invalidate();
        DLOG("propagate: {} → {}", var, def);
        return def;
    }

    auto cur = i->second;
    if (!cur || def->isa<Bot>() || cur == def || cur == var || isa_gvn_proxy(cur)) return cur;

    invalidate();
    DLOG("cannot propagate {}, trying GVN", var);
    if (cur->isa<Bot>()) return i->second = def;
    return i->second = nullptr; // we reached top for propagate; nullptr marks this to bundle for GVN
}

DefVec SymExprOpt::Analysis::sccp_gvn_propagate(DefVec& concr_vars, DefVec& abstr_args) {
    auto n_all = concr_vars.size();
    assert(concr_vars.size() == abstr_args.size());

    DefVec abstr_vars;
    for (size_t i = 0; i < concr_vars.size(); i++)
        abstr_vars.emplace_back(sccp_join(concr_vars[i], abstr_args[i]));

    DefMap<size_t> var2index;
    for (size_t i = 0; auto var : concr_vars)
        var2index[var] = i++;

    // GVN bundle: All things marked as top (nullptr) by propagate are now treated as one entity by bundling
    // them into one proxy
    for (size_t i = 0; i != n_all; ++i) {
        if (abstr_vars[i]) continue;

        auto bundle_vars = DefVec();
        bundle_vars.emplace_back(concr_vars[i]);

        for (size_t j = i + 1; j != n_all; ++j)
            if (!abstr_vars[j] && abstr_args[j] == abstr_args[i]) bundle_vars.emplace_back(concr_vars[j]);

        if (bundle_vars.size() == 1) {
            lattice_[concr_vars[i]] = abstr_vars[i] = concr_vars[i]; // top
        } else {
            auto proxy = world().proxy(concr_vars[i]->type(), bundle_vars, 0, Proxy_GVN);

            for (auto p : proxy->ops()) {
                auto j                  = var2index[p];
                lattice_[concr_vars[j]] = abstr_vars[j] = proxy;
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
    for (size_t i = 0; i != n_all; ++i) {
        if (auto proxy = isa_gvn_proxy(abstr_vars[i])) {
            auto num        = proxy->num_ops();
            auto split_vars = DefVec();

            for (auto p : proxy->ops()) {
                auto j = var2index[p];
                if (p == concr_vars[j] && abstr_args[i] == abstr_args[j]) split_vars.emplace_back(concr_vars[j]);
            }

            auto new_num = split_vars.size();
            if (new_num == 1) {
                invalidate();
                lattice_[concr_vars[i]] = abstr_vars[i] = concr_vars[i];
                DLOG("single: {}", concr_vars[i]);
            } else if (new_num != num) {
                invalidate();
                auto new_proxy = world().proxy(abstr_args[i]->type(), split_vars, 0, Proxy_GVN);
                DLOG("split: {}", new_proxy);

                for (auto p : new_proxy->ops()) {
                    auto j = var2index[p];
                    if (p == concr_vars[j]) lattice_[concr_vars[j]] = abstr_vars[j] = new_proxy;
                }
            }
            // if new_num == num: do nothing
        }
    }

    return abstr_vars;
}

const Def* SymExprOpt::Analysis::rewrite_imm_App(const App* app) {
    if (auto store = Axm::isa<mem::store>(app)) {
        auto [mem, ptr, val] = store->args<3>();
        auto abstr_mem       = rewrite(mem);
        auto abstr_ptr       = rewrite(ptr);
        auto abstr_val       = rewrite(val);
        // Only track values stored through slot proxies; arbitrary pointers may alias each other.
        if (isa_slot_proxy(abstr_ptr)) {
            slot2value(abstr_ptr, abstr_val);
            DLOG("in {}, found a store: {} <- {}", curr_mut(), abstr_ptr, abstr_val);
        }
        analyze(abstr_val); // a slot stored *as value* escapes
        return abstr_mem;
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, as]                   = load->decurry()->args<2>();
        auto [result_mem, result_load] = load->projs<2>();
        auto [mem, ptr]                = load->args<2>();
        rewrite(mem);
        auto abstr_ptr = rewrite(ptr);
        DLOG("in {}, found a load from {}", curr_mut(), abstr_ptr);
        if (isa_slot_proxy(abstr_ptr)) {
            if (auto known_value = slot2value(abstr_ptr)) {
                DLOG("we know that it's {}", known_value);
                set(result_load, known_value);
                return world().tuple({result_mem, known_value});
            }
            DLOG("couldn't resolve load of {} yet, returning bot", abstr_ptr);
            return world().tuple({result_mem, world().bot(T)});
        }
        // A load through an arbitrary pointer never resolves; its result is top - not bot -
        // as bot would be propagated as a concrete value.
        // set() overwrites a possibly stale value from an earlier round in which the pointer was still a slot proxy.
        DLOG("load from unknown pointer {}, treating result as top", abstr_ptr);
        set(result_load, result_load);
        return world().tuple({result_mem, result_load});
    } else if (auto slot = Axm::isa<mem::slot>(app)) {
        // if the slot is top (address taken), it escapes and must be kept as is
        if (auto i = lattice_.find(slot); i != lattice_.end() && i->second == slot)
            return mim::Analysis::rewrite_imm_App(app);

        auto [Ta, mi]   = slot->uncurry_args<2>();
        auto [T, as]    = Ta->projs<2>();
        auto [mem, id]  = mi->projs<2>();
        auto [_, ptr]   = slot->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_id   = rewrite(id);
        all_slots_[ptr] = T;
        // The sloxy is Ptr-typed so that any use outside of load/store still type-checks;
        // analyze() catches such leaked sloxies afterwards and sets the slot to top.
        auto sloxy         = world().proxy(ptr->type(), {curr_mut(), abstr_id}, 0, Proxy_Slot);
        sloxy2slot_[sloxy] = slot;
        DLOG("in {}, found declaration for slot {}", curr_mut(), ptr);
        set(ptr, sloxy);
        return world().tuple({abstr_mem, sloxy});
    } else if (auto branch = Branch(app)) {
        DefVec abstr_args;
        for (auto arg : app->targs())
            abstr_args.emplace_back(rewrite(arg));

        auto abstr_cond = rewrite(branch.cond());
        auto l          = Lit::isa<bool>(abstr_cond);
        DLOG("abstract value of branch cond {} in {}: {}", branch.cond(), curr_mut(), abstr_cond);

        for (auto [slot, slot_type] : all_slots_) {
            auto abstr_slot = rewrite(slot);
            if (auto value = slot2value(abstr_slot)) abstr_args.emplace_back(value);
        }

        auto tt = branch.tt()->isa_mut<Lam>();
        auto ff = branch.ff()->isa_mut<Lam>();

        // Propagate into both sides - even if the condition is abstractly known:
        // a side skipped in one fixed-point round would accumulate a lattice - and hence a signature -
        // that differs from its sibling's, but SymExprOpt::rewrite_imm_App relies on both sides agreeing.
        // For the same reason propagate either into both sides or into none.
        bool propagated = isa_optimizable(tt) && isa_optimizable(ff);
        if (propagated) {
            for (auto lam : {tt, ff}) {
                DLOG("branch, writing local values to {}", lam);

                DefVec concr_vars;
                for (auto var : lam->tvars())
                    concr_vars.emplace_back(var);

                for (auto [slot, slot_type] : all_slots_) {
                    auto abstr_slot = rewrite(slot);
                    auto phi        = world().proxy(slot_type, {lam, abstr_slot}, 0, Proxy_Phi);
                    if (slot2value(abstr_slot)) {
                        concr_vars.emplace_back(phi);
                    } else {
                        DLOG("no value found for {}", slot);
                        // TODO Can this ever happen?
                    }
                }

                auto abstr_vars = sccp_gvn_propagate(concr_vars, abstr_args);
                set(lam->var(), world().tuple(abstr_vars.span().subspan(0, app->num_targs())));
                for (size_t i = 0; i < concr_vars.size(); i++)
                    set(concr_vars[i], abstr_vars[i]);
            }
        }

        // Propagated sides must be traversed via rewrite_deps() to keep their lattice state;
        // everything else (Var%s, externals, ...) escapes normally.
        auto rewrite_side = [&](const Def* side) -> const Def* {
            if (propagated) return rewrite_deps(side->as_mut());
            return rewrite(side);
        };

        if (l && !*l) {
            auto abstr_ff = rewrite_side(branch.ff());
            return world().app(abstr_ff, abstr_args.span().subspan(0, app->num_targs()));
        } else if (l && *l) {
            auto abstr_tt = rewrite_side(branch.tt());
            return world().app(abstr_tt, abstr_args.span().subspan(0, app->num_targs()));
        } else {
            auto abstr_ff = rewrite_side(branch.ff());
            auto abstr_tt = rewrite_side(branch.tt());
            return world().app(world().extract(world().tuple({abstr_ff, abstr_tt}), abstr_cond),
                               abstr_args.span().subspan(0, app->num_targs()));
        }
    } else if (auto lam = app->callee()->isa_mut<Lam>(); lam && !isa_optimizable(lam)) {
        DLOG("{} not optimizable", app->callee());

        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);

        for (size_t i = 0; i != n; ++i) {
            if (auto continuation = app->targ(i)->isa_mut<Lam>(); isa_optimizable(continuation)) {
                // need to rewrite this later, after slot values in the lattice are updated
                abstr_args[i] = continuation;
            } else {
                DLOG("in app of non-optimizable function, rewriting arg {}", app->targ(i));
                abstr_args[i] = rewrite(app->targ(i));
            }
        }

        for (size_t i = 0; i != n; ++i) {
            if (auto continuation = app->targ(i)->isa_mut<Lam>(); isa_optimizable(continuation)) {
                // now propagate known slot values and rewrite the continuation
                DefVec concr_vars_inside_unknown_function;
                DefVec abstr_args_inside_unknown_function;

                // set the continuations arguments to top, as we don't know what the unknown function will pass
                for (auto var : continuation->tvars()) {
                    concr_vars_inside_unknown_function.emplace_back(var);
                    abstr_args_inside_unknown_function.emplace_back(var);
                }

                // propagate known slot contents
                // some of these might escape, but we'll catch that in the postprocessing later
                for (auto [slot, slot_type] : all_slots_) {
                    auto abstr_slot = rewrite(slot);
                    auto phi        = world().proxy(slot_type, {continuation, abstr_slot}, 0, Proxy_Phi);
                    if (auto value = slot2value(abstr_slot)) {
                        concr_vars_inside_unknown_function.emplace_back(phi);
                        abstr_args_inside_unknown_function.emplace_back(value);
                    } else {
                        DLOG("no value found for {}", slot);
                        // TODO Can this ever happen?
                        // propagate(lam_proxy, lam_proxy);
                    }
                }

                auto abstr_vars_inside_unknown_function
                    = sccp_gvn_propagate(concr_vars_inside_unknown_function, abstr_args_inside_unknown_function);
                set(continuation->var(),
                    world().tuple(abstr_vars_inside_unknown_function.span().subspan(0, continuation->num_tvars())));
                for (size_t i = 0; i < concr_vars_inside_unknown_function.size(); i++)
                    set(concr_vars_inside_unknown_function[i], abstr_vars_inside_unknown_function[i]);

                // now rewrite the continuation
                abstr_args[i] = rewrite(app->targ(i));
            }
        }

        return world().app(rewrite_deps(lam), abstr_args);
    } else if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
        DefVec all_concr_vars;
        DefVec all_abstr_args;

        // propagate vars
        for (size_t i = 0; i != app->num_targs(); ++i) {
            all_concr_vars.emplace_back(lam->tvar(i));
            all_abstr_args.emplace_back(rewrite(app->targ(i)));
        }

        // propagate phis
        DLOG("propagating slot values for call of {}", lam);
        for (auto [slot, slot_type] : all_slots_) {
            DLOG("for slot {}", slot);
            auto abstr_slot = rewrite(slot);
            auto phi        = world().proxy(slot_type, {lam, abstr_slot}, 0, Proxy_Phi);
            if (auto value = slot2value(abstr_slot)) {
                all_concr_vars.emplace_back(phi);
                all_abstr_args.emplace_back(value);
            } else {
                DLOG("no value found for {}", slot);
                // TODO Can this ever happen?
                // propagate(lam_proxy, lam_proxy);
            }
        }

        auto all_abstr_vars = sccp_gvn_propagate(all_concr_vars, all_abstr_args);

        set(lam->var(), world().tuple(all_abstr_vars.span().subspan(0, app->num_targs())));
        for (size_t i = app->num_targs(); i < all_concr_vars.size(); i++)
            set(all_concr_vars[i], all_abstr_vars[i]);

        return world().app(rewrite_deps(lam), all_abstr_args.span().subspan(0, app->num_targs()));
    }

    return mim::Analysis::rewrite_imm_App(app);
}

static bool keep(const Def* old_var, const Def* abstr) {
    if (!abstr) return true;           // no info -> keep
    if (old_var == abstr) return true; // top
    if (auto proxy = isa_gvn_proxy(abstr))
        // TODO: if old_var is a phi, only keep it if all the entries in the bundle are phis. if not, prefer the first
        // non-phi
        return proxy->op(0) == old_var; // first in GVN bundle
    else
        return false;
}

const Def* SymExprOpt::rewrite_imm_App(const App* old_app) {
    if (auto slot = Axm::isa<mem::slot>(old_app)) {
        auto [mem, id] = slot->args<2>();
        auto [_, ptr]  = slot->projs<2>();
        if (auto sloxy = lattice(ptr); sloxy && sloxy != ptr) {
            auto rewritten_mem = rewrite(mem);
            return new_world().tuple(
                {rewritten_mem,
                 new_world().bot(
                     rewrite(ptr->type()))}); // return bot for the pointer, we hopefully proved that no one uses it
        }
    } else if (auto store = Axm::isa<mem::store>(old_app)) {
        auto [mem, ptr, val] = store->args<3>();
        if (auto sloxy = lattice(ptr); sloxy && sloxy != ptr) return rewrite(mem);
    } else if (auto load = Axm::isa<mem::load>(old_app)) {
        auto [result_mem, result_load] = load->projs<2>();
        auto [mem, ptr]                = load->args<2>();
        DLOG("rewriting a load from {} ({})", ptr, old_app);
        if (auto known_load = lattice(result_load); known_load && known_load != result_load) {
            DLOG("rewriting a load from {}, we know that it's {}", ptr, known_load);
            auto rewritten_mem = rewrite(mem);
            return new_world().tuple({rewritten_mem, rewrite(known_load)});
        }
    } else if (auto branch = Branch(old_app)) {
        auto old_tt = branch.tt()->isa_mut<Lam>();
        auto old_ff = branch.ff()->isa_mut<Lam>();
        if (old_tt && old_ff && (needs_seo(old_tt) || needs_seo(old_ff))) {
            auto new_cond = rewrite(branch.cond());
            DLOG("in {}, found branch on {} ({}) with {} / {}", curr_mut(), branch.cond(), new_cond, old_ff, old_tt);

            if (auto l = Lit::isa<bool>(new_cond)) {
                // the branch folds in the new world; only rebuild the taken side
                auto old_taken = *l ? old_tt : old_ff;
                if (needs_seo(old_taken)) {
                    invalidate();
                    return map(old_app, new_world().app(build_lam(old_taken), build_args(old_taken, old_app)));
                }
                return RWPhase::rewrite_imm_App(old_app);
            }

            invalidate();
            auto tt_args = build_args(old_tt, old_app);
            auto ff_args = build_args(old_ff, old_app);
            auto new_tt  = needs_seo(old_tt) ? build_lam(old_tt) : rewrite(old_tt);
            auto new_ff  = needs_seo(old_ff) ? build_lam(old_ff) : rewrite(old_ff);

            // EtaExpPhase runs beforehand, so both sides are fresh single-use Lams that received the same
            // propagation; hence their rebuilt signatures and arguments agree.
            assert(tt_args == ff_args && new_tt->type() == new_ff->type());
            auto new_callee = new_world().extract(new_world().tuple({new_ff, new_tt}), new_cond);
            return map(old_app, new_world().app(new_callee, tt_args));
        }
    } else if (auto old_lam = old_app->callee()->isa_mut<Lam>(); old_lam && !isa_optimizable(old_lam)) {
        // Continuations passed to an unknown callee cannot receive extra args for their phis;
        // resolve each phi to the value known at this call site instead.
        for (auto old_arg : old_app->targs()) {
            auto cont = old_arg->isa_mut<Lam>();
            if (!cont) continue;

            for (auto [slot, slot_type] : analysis_.all_slots()) {
                if (auto sloxy = lattice(slot)) {
                    auto phi = old_world().proxy(slot_type, {cont, sloxy}, 0, Proxy_Phi);
                    if (auto def = lattice(phi)) {
                        auto new_val = rewrite_site_value(sloxy, slot_type);
                        DLOG("in {}, resolving phi {} of continuation {} to {}", curr_mut(), phi, cont, new_val);
                        map(phi, new_val);
                        if (def != phi && isa_gvn_proxy(def)) map(def, new_val);
                    }
                }
            }
        }
    } else if (auto old_lam = old_app->callee()->isa_mut<Lam>()) {
        DLOG("in {}, found app of {}", curr_mut(), old_app->callee());
        if (needs_seo(old_lam)) {
            invalidate();
            auto new_lam  = build_lam(old_lam);
            auto new_args = build_args(old_lam, old_app);
            return map(old_app, new_world().app(new_lam, new_args));
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

bool SymExprOpt::needs_seo(Lam* old_lam) {
    if (auto l = lattice(old_lam->var()); l && l != old_lam->var()) return true;

    for (auto [slot, slot_type] : analysis_.all_slots()) {
        if (auto sloxy = lattice(slot)) {
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, 0, Proxy_Phi);
            if (auto def = lattice(phi); def && keep(phi, def)) return true;
        }
    }

    return false;
}

Lam* SymExprOpt::build_lam(Lam* old_lam) {
    if (auto i = lam2lam_.find(old_lam); i != lam2lam_.end()) return i->second;

    DLOG("building a new lam for {}", old_lam);

    // find phis
    Vector<std::pair<const Def*, const Def*>> potential_phis;
    for (auto [slot, slot_type] : analysis_.all_slots()) {
        if (auto sloxy = lattice(slot)) {
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, 0, Proxy_Phi);
            if (auto def = lattice(phi)) potential_phis.push_back({phi, def});
        }
    }

    size_t num_old = old_lam->num_tvars();

    // build new dom
    auto new_doms = DefVec();
    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);
        if (keep(old_var, abstr)) new_doms.emplace_back(rewrite(old_lam->dom(num_old, i)));
    }

    for (auto [proxy, abstr] : potential_phis)
        if (keep(proxy, abstr)) new_doms.emplace_back(rewrite(proxy->type()));

    size_t num_new_vars = new_doms.size();

    // build new lam
    auto var_map      = absl::FixedArray<const Def*>(num_old);
    auto new_lam      = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
    lam2lam_[old_lam] = new_lam;

    // build new var
    size_t j = 0;

    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);

        if (keep(old_var, abstr)) {
            auto v     = new_lam->var(num_new_vars, j++);
            var_map[i] = v;
            if (abstr && abstr != old_var) map(abstr, v); // GVN bundle
        } else {
            var_map[i] = rewrite(abstr); // SCCP propagate
        }
    }

    for (size_t i = 0; i < potential_phis.size(); i++) {
        auto [proxy, abstr] = potential_phis[i];
        DLOG("deciding if we keep the phi: {}, {}", proxy, abstr);
        if (keep(proxy, abstr)) {
            auto v = new_lam->var(num_new_vars, j++);
            DLOG("mapping phi {} to {}", proxy, v);
            map(proxy, v);
            if (abstr != proxy) {
                DLOG("need to map a phi to gvn bundle");
                map(abstr, v);
            }
        } else {
            DLOG("need to map a phi to constant value");
            map(proxy, rewrite(abstr));
        }
    }

    map(old_lam->var(), var_map);
    {
        auto _ = enter(old_lam);
        new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
    }

    return new_lam;
}

DefVec SymExprOpt::build_args(Lam* old_lam, const App* old_app) {
    size_t num_old = old_lam->num_tvars();
    auto new_args  = DefVec();

    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);
        if (keep(old_var, abstr)) new_args.emplace_back(rewrite(old_app->targ(i)));
    }

    DLOG("wiring up phi arguments");
    for (auto [slot, slot_type] : analysis_.all_slots()) {
        if (auto sloxy = lattice(slot)) {
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, 0, Proxy_Phi);
            if (auto def = lattice(phi); def && keep(phi, def))
                new_args.emplace_back(rewrite_site_value(sloxy, slot_type));
        }
    }

    return new_args;
}

const Def* SymExprOpt::rewrite_site_value(const Def* sloxy, const Def* slot_type) {
    if (auto slot2value_it = analysis_.mut2slot2value().find(curr_mut());
        slot2value_it != analysis_.mut2slot2value().end()) {
        auto& slot2value = slot2value_it->second;
        if (auto found_value_it = slot2value.find(sloxy); found_value_it != slot2value.end())
            return rewrite(found_value_it->second);
    }
    // curr_mut() didn't write to the slot; forward our own phi for it
    auto curr_phi = old_world().proxy(slot_type, {curr_mut(), sloxy}, 0, Proxy_Phi);
    return rewrite(curr_phi);
}

} // namespace mim::plug::mem::phase
