#include "mim/plug/mem/phase/sym_expr_opt.h"

#include <absl/container/fixed_array.h>

#include "mim/def.h"
#include "mim/phase.h"

#include "mim/plug/mem/mem.h"

template<std::ranges::input_range... Rs>
auto concat_to_vector(Rs&&... rs) {
    using T = std::common_type_t<std::ranges::range_value_t<Rs>...>;
    mim::Vector<T> out;

    (std::ranges::copy(rs, std::back_inserter(out)), ...);

    return out;
}

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

static const Def* isa_phi_proxy(const Def* def) {
    if (auto mem = def->isa<Proxy>())
        if (mem->tag() == Proxy_Phi) return mem;
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
}

void SymExprOpt::Analysis::start() {
    mim::Analysis::start();
    for (auto def : world().roots())
        analyze(def);
}

void SymExprOpt::Analysis::analyze(const Def* def) {
    if (auto l = lookup(def)) def = l;

    if (auto sloxy = isa_slot_proxy(def)) {
        auto slot = sloxy2slot_[sloxy];
        set(slot, slot);
        DLOG("setting slot to top: {}", slot);
        invalidate();
    }

    if (auto [_, ins] = visited_.emplace(def); ins)
        for (auto d : def->deps())
            analyze(d);
}

const Def* SymExprOpt::Analysis::propagate(const Def* var, const Def* def) {
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

const Def* SymExprOpt::Analysis::rewrite_imm_App(const App* app) {
    DLOG("imm_app of {}, currently in {}", app->callee(), curr_mut());
    DLOG("callee {} is a mut? {}", app->callee(), app->callee()->isa_mut());
    DLOG("callee {} is a var? {}", app->callee(), app->callee()->isa<Var>());
    if (auto store = Axm::isa<mem::store>(app)) {
        auto [mem, ptr, val] = store->args<3>();
        auto abstr_mem       = rewrite(mem);
        auto abstr_ptr       = rewrite(ptr);
        auto abstr_val       = rewrite(val);
        slot2value(abstr_ptr, abstr_val);
        DLOG("in {}, found a store: {} <- {}", curr_mut(), abstr_ptr, abstr_val);
        return abstr_mem;
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, as]                   = load->decurry()->args<2>();
        auto [result_mem, result_load] = load->projs<2>();
        auto [mem, ptr]                = load->args<2>();
        rewrite(mem);
        auto abstr_ptr = rewrite(ptr);
        DLOG("in {}, found a load from {}", curr_mut(), abstr_ptr);
        if (auto known_value = slot2value(abstr_ptr)) {
            DLOG("we know that it's {}", known_value);
            set(result_load, known_value);
            return world().tuple({result_mem, known_value});
        } else {
            DLOG("still resolving load, looking up {} in lattice", abstr_ptr);
            auto phi = world().proxy(T, {curr_mut(), abstr_ptr}, 0, Proxy_Phi);
            if (auto it = lattice_.find(phi); it != lattice_.end()) {
                set(result_load, it->second);
                return world().tuple({result_mem, it->second});
            } else {
                DLOG("couldn't resolve load of {}, returning bot", abstr_ptr);
                return world().tuple({result_mem, world().bot(T)});
            }
        }
    } else if (auto slot = Axm::isa<mem::slot>(app)) {
        auto [Ta, mi]   = slot->uncurry_args<2>();
        auto [T, as]    = Ta->projs<2>();
        auto [mem, id]  = mi->projs<2>();
        auto [_, ptr]   = slot->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_id   = rewrite(id);
        all_slots_[ptr] = T;
        // TODO if top (address taken), don't do that
        auto sloxy         = world().proxy(T, {curr_mut(), abstr_id}, 0, Proxy_Slot);
        sloxy2slot_[sloxy] = slot;
        DLOG("in {}, found declaration for slot {}", curr_mut(), ptr);
        set(ptr, sloxy);
        return world().tuple({abstr_mem, sloxy});
    } else if (auto branch = Branch(app)) {
        auto abstr = rewrite(branch.cond());
        auto l     = Lit::isa<bool>(abstr);
        DLOG("abstract value of branch cond {} in {}: {}", branch.cond(), curr_mut(), abstr);
        if (!l || *l)
            if (auto lam = branch.tt()->isa_mut<Lam>(); isa_optimizable(lam)) {
                DLOG("branch, writing local values to {}", lam);
                for (auto [slot, slot_type] : all_slots_) {
                    auto abstr_slot = rewrite(slot);
                    if (auto value = slot2value(abstr_slot)) {
                        mut2slot2value_[lam][abstr_slot] = value;
                    } else {
                        auto phi = world().proxy(slot_type, {curr_mut(), abstr_slot}, 0, Proxy_Phi);
                        DLOG("looking up {}", phi);
                        if (auto it = lattice_.find(phi); it != lattice_.end())
                            mut2slot2value_[lam][abstr_slot] = it->second;
                    }
                }
            }
        if (!l || !*l)
            if (auto lam = branch.ff()->isa_mut<Lam>(); isa_optimizable(lam)) {
                DLOG("branch, writing local values to {}", lam);
                for (auto [slot, slot_type] : all_slots_) {
                    auto abstr_slot = rewrite(slot);
                    if (auto value = slot2value(abstr_slot)) {
                        mut2slot2value_[lam][abstr_slot] = value;
                    } else {
                        auto phi = world().proxy(slot_type, {curr_mut(), abstr_slot}, 0, Proxy_Phi);
                        if (auto it = lattice_.find(phi); it != lattice_.end())
                            mut2slot2value_[lam][abstr_slot] = it->second;
                    }
                }
            }
    } else if (auto lam = app->callee()->isa_mut<Lam>(); lam && !isa_optimizable(lam)) {
        // TODO: this works differently now, check what needs updating
        DLOG("{} not optimizable", app->callee());

        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);

        for (auto arg : app->targs()) {
            DLOG("arg {} of {}, free vars:", arg, app->callee());
            for (auto v : arg->free_vars())
                DLOG("{}", v);
        }

        for (size_t i = 0; i != n; ++i)
            if (auto continuation = app->targ(i)->isa_mut<Lam>(); isa_optimizable(continuation)) {
                // need to rewrite this later, after local slot values are updated
                // TODO: think about this more
                abstr_args[i] = continuation;
            } else {
                DLOG("in app of non-optimizable function, rewriting arg {}", app->targ(i));
                abstr_args[i] = rewrite(app->targ(i));
            }

        // DLOG("done analyzing args of {}", lam);

        bool mem_passed = false;
        // TODO: this all doesn't work if the arguments are "hidden" in some way, like if you pass a slot inside a tuple or a map.
        if (mem_passed) {
            DLOG("a mem is passed to {}", lam);
            for (auto arg : abstr_args) {
                if (auto continuation = arg->isa_mut<Lam>(); isa_optimizable(continuation)) {
                    DLOG("a continuation is passed to {}", lam);
                    // The unknown function may call this as a continuation. In that case, the slot
                    // values are the same as for the current function.
                    for (auto [slot, slot_type] : all_slots_) {
                        auto abstr_slot = rewrite(slot);
                        if (auto value = slot2value(abstr_slot)) {
                            mut2slot2value_[continuation][abstr_slot] = value;
                        } else {
                            auto phi = world().proxy(slot_type, {curr_mut(), abstr_slot}, 0, Proxy_Phi);
                            DLOG("looking up {}", phi);
                            if (auto it = lattice_.find(phi); it != lattice_.end())
                                mut2slot2value_[continuation][abstr_slot] = it->second;
                        }
                    }

                    // Except for those slots that are also passed to the unknown function. We don't
                    // know what it does to those, so we set them to top.
                    for (auto [slot, slot_type] : all_slots_) {
                        auto abstr_slot = rewrite(slot);
                        if (auto i = std::find(abstr_args.begin(), abstr_args.end(), abstr_slot);
                            i != abstr_args.end()) {
                            DLOG("{} passed as continuation, and {} escapes, setting to top", continuation, arg);
                            mut2slot2value_[continuation][abstr_slot] = mut2slot2value_[continuation][abstr_slot];
                        }
                    }

                    // TODO: do we need to set todo_ or call rewrite again or something?
                }
            }
        }

        // TODO: think about this more
        for (size_t i = 0; i != n; ++i)
            if (auto continuation = app->targ(i)->isa_mut<Lam>(); isa_optimizable(continuation))
                abstr_args[i] = rewrite(app->targ(i));

        return world().app(rewrite_deps(lam), abstr_args);
    } else if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);
        auto abstr_vars = absl::FixedArray<const Def*>(n);

        // propagate vars
        for (size_t i = 0; i != n; ++i) {
            auto abstr    = rewrite(app->targ(i));
            abstr_vars[i] = propagate(lam->tvar(i), abstr);
            abstr_args[i] = abstr;
        }

        // propagate phis
        DLOG("propagating slot values for call of {}", lam);
        Vector<const Def*> concr_phis;
        Vector<const Def*> abstr_phis;
        Vector<const Def*> phi_args;
        for (auto [slot, slot_type] : all_slots_) {
            DLOG("for slot {}", slot);
            auto abstr_slot = rewrite(slot);
            auto phi        = world().proxy(slot_type, {lam, abstr_slot}, 0, Proxy_Phi);
            if (auto value = slot2value(abstr_slot)) {
                auto abstr = propagate(phi, value);
                phi_args.emplace_back(value);
                concr_phis.emplace_back(phi);
                abstr_phis.emplace_back(abstr);
                DLOG("propagating local value: {} -> {}", slot, value);
                set(phi, abstr);
            } else {
                DLOG("not found in slot2value, propagating value from parameters");
                auto lookup_proxy = world().proxy(slot_type, {curr_mut(), abstr_slot}, 0, Proxy_Phi);
                if (auto i = lattice_.find(lookup_proxy); i != lattice_.end()) {
                    auto value = i->second;
                    auto abstr = propagate(phi, value);
                    phi_args.emplace_back(value);
                    concr_phis.emplace_back(phi);
                    abstr_phis.emplace_back(abstr);
                    DLOG("propagating value from phi: {} -> {}", slot, value);
                    set(phi, abstr);
                } else {
                    DLOG("no value found for {}", slot);
                    // TODO Can this ever happen?
                    // propagate(lam_proxy, lam_proxy);
                }
            }
        }

        auto all_concr_vars = concat_to_vector(lam->tvars(), concr_phis);
        auto all_abstr_vars = concat_to_vector(abstr_vars, abstr_phis);
        auto all_abstr_args = concat_to_vector(abstr_args, phi_args);
        auto n_all          = all_abstr_vars.size();
        assert(all_concr_vars.size() == all_abstr_vars.size() && all_abstr_vars.size() == all_abstr_args.size());

        DefMap<size_t> var2index;
        for (size_t i = 0; auto var : all_concr_vars)
            var2index[var] = i++;

        // GVN bundle: All things marked as top (nullptr) by propagate are now treated as one entity by bundling
        // them into one proxy
        for (size_t i = 0; i != n_all; ++i) {
            if (all_abstr_vars[i]) continue;

            auto bundle_vars = DefVec();
            auto vi          = all_concr_vars[i];
            auto ai          = all_abstr_args[i];
            bundle_vars.emplace_back(vi);

            for (size_t j = i + 1; j != n_all; ++j) {
                auto vj = all_concr_vars[j];
                if (!all_abstr_vars[j] && all_abstr_args[j] == ai) bundle_vars.emplace_back(vj);
            }

            if (bundle_vars.size() == 1) {
                lattice_[vi] = all_abstr_vars[i] = vi; // top
            } else {
                auto proxy = world().proxy(vi->type(), bundle_vars, 0, Proxy_GVN);

                for (auto p : proxy->ops()) {
                    auto j       = var2index[p];
                    auto vj      = all_concr_vars[j];
                    lattice_[vj] = all_abstr_vars[j] = proxy;
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
            if (auto proxy = isa_gvn_proxy(all_abstr_vars[i])) {
                auto num        = proxy->num_ops();
                auto split_vars = DefVec();
                auto ai         = all_abstr_args[i];

                for (auto p : proxy->ops()) {
                    auto j  = var2index[p];
                    auto vj = all_concr_vars[j];
                    if (p == vj) {
                        if (ai == all_abstr_args[j]) split_vars.emplace_back(vj);
                    }
                }

                auto new_num = split_vars.size();
                if (new_num == 1) {
                    invalidate();
                    auto vi      = all_concr_vars[i];
                    lattice_[vi] = all_abstr_vars[i] = vi;
                    DLOG("single: {}", vi);
                } else if (new_num != num) {
                    invalidate();
                    auto new_proxy = world().proxy(ai->type(), split_vars, 0, Proxy_GVN);
                    DLOG("split: {}", new_proxy);

                    for (auto p : new_proxy->ops()) {
                        auto j  = var2index[p];
                        auto vj = all_concr_vars[j];
                        if (p == vj) lattice_[vj] = all_abstr_vars[j] = new_proxy;
                    }
                }
                // if new_num == num: do nothing
            }
        }

        set(lam->var(), world().tuple(all_abstr_vars.span().subspan(0, n)));
        return world().app(rewrite_deps(lam), abstr_args);
    }

    return mim::Analysis::rewrite_imm_App(app);
}

static bool keep(const Def* old_var, const Def* abstr) {
    if (old_var == abstr) return true; // top
    if (auto proxy = isa_gvn_proxy(abstr))
        return proxy->op(0) == old_var; // first in GVN bundle
    else
        return false;
}

const Def* SymExprOpt::rewrite_imm_App(const App* old_app) {
    if (auto slot = Axm::isa<mem::slot>(old_app)) {
        auto [Ta, mi]  = slot->uncurry_args<2>();
        auto [T, as]   = Ta->projs<2>();
        auto [mem, id] = mi->projs<2>();
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
        if (auto known_load = lattice(result_load)) {
            DLOG("rewriting a load from {}, we know that it's {}", ptr, known_load);
            auto rewritten_mem = rewrite(mem);
            return new_world().tuple({rewritten_mem, rewrite(known_load)});
        }
    } else if (auto old_lam = old_app->callee()->isa_mut<Lam>()) {
        DLOG("in {}, found app of {}", curr_mut(), old_app->callee());
        bool phis_needed = false;
        for (auto [slot, slot_type] : analysis_.all_slots()) {
            if (auto sloxy = lattice(slot)) {
                auto phi = old_world().proxy(slot_type, {old_app->callee(), sloxy}, 0, Proxy_Phi);
                if (auto def = lattice(phi); def && keep(phi, def)) phis_needed = true;
            }
        }
        DLOG("phis_needed: {}", phis_needed);
        if (auto l = lattice(old_lam->var()); (l && l != old_lam->var()) || phis_needed) {
            DLOG("building a new lam for {}", old_app->callee());
            invalidate();

            // find phis
            Vector<std::pair<const Def*, const Def*>> potential_phis;
            for (auto [slot, slot_type] : analysis_.all_slots()) {
                if (auto sloxy = lattice(slot)) {
                    auto phi = old_world().proxy(slot_type, {old_app->callee(), sloxy}, 0, Proxy_Phi);
                    if (auto def = lattice(phi)) potential_phis.push_back({phi, def});
                }
            }

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
                size_t num_kept_vars = new_doms.size();

                for (auto [proxy, abstr] : potential_phis)
                    if (keep(proxy, abstr)) new_doms.emplace_back(rewrite(proxy->type()));

                size_t num_new_vars = new_doms.size();

                // build new lam
                auto mapped_vars  = absl::FixedArray<const Def*>(num_kept_vars);
                new_lam           = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
                lam2lam_[old_lam] = new_lam;

                // build new var
                size_t j = 0;

                for (size_t i = 0; i != num_old; ++i) {
                    auto old_var = old_lam->var(num_old, i);
                    auto abstr   = lattice(old_var);

                    if (keep(old_var, abstr)) {
                        auto v         = new_lam->var(num_new_vars, j++);
                        mapped_vars[i] = v;
                        if (abstr != old_var) map(abstr, v); // GVN bundle
                    } else {
                        mapped_vars[i] = rewrite(abstr); // SCCP propagate
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

                map(old_lam->var(), mapped_vars);
                new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
            }

            // build new app
            size_t num_new = new_lam->num_vars();
            auto new_args  = absl::FixedArray<const Def*>(num_new);

            size_t j = 0;
            for (size_t i = 0; i != num_old; ++i) {
                auto old_var = old_lam->var(num_old, i);
                auto abstr   = lattice(old_var);
                if (keep(old_var, abstr)) new_args[j++] = rewrite(old_app->targ(i));
            }

            DLOG("wiring up phi arguments");
            for (auto [mut, slot2value] : analysis_.mut2slot2value()) {
                DLOG("known values  for mut {}:", mut);
                for (auto [slot, value] : slot2value)
                    DLOG("  {} -> {}", slot, value);
            }

            for (auto [slot, slot_type] : analysis_.all_slots()) {
                if (auto sloxy = lattice(slot)) {
                    auto phi = old_world().proxy(slot_type, {old_app->callee(), sloxy}, 0, Proxy_Phi);
                    if (auto def = lattice(phi)) {
                        if (keep(phi, def)) {
                            if (auto slot2value_it = analysis_.mut2slot2value().find(curr_mut());
                                slot2value_it != analysis_.mut2slot2value().end()) {
                                auto slot2value = slot2value_it->second;
                                if (auto found_value_it = slot2value.find(sloxy); found_value_it != slot2value.end()) {
                                    auto found_value = found_value_it->second;
                                    new_args[j++]    = rewrite(found_value);
                                } else {
                                    auto phi      = old_world().proxy(slot_type, {curr_mut(), sloxy}, 0, Proxy_Phi);
                                    new_args[j++] = rewrite(phi);
                                }
                            }
                        }
                    }
                }
            }

            auto result = map(old_app, new_world().app(new_lam, new_args));
            return result;
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim::plug::mem::phase
