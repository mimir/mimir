#include "mim/plug/mem/phase/seo.h"

#include <absl/container/fixed_array.h>

#include <mim/lam.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

/*
 * Helpers
 */

enum {
    Proxy_GVN,
    Proxy_Slot,
    Proxy_Phi,
};

void SEO::Analysis::reset() {
    Super::reset();
    visited_.clear();
    mut2slot2value_.clear();
    deps_done_.clear();
}

Def* SEO::Analysis::rewrite_deps(Def* mut) {
    if (auto [_, ins] = deps_done_.emplace(mut); !ins) return mut;
    return Super::rewrite_deps(mut);
}

const Def* SEO::Analysis::slot2value(const Def* slot) {
    const auto& slot2value = mut2slot2value_[curr_mut()];
    if (auto i = slot2value.find(slot); i != slot2value.end()) return i->second;

    // not in the local map: check if we have a phi for this slot in the lattice
    auto [T, _] = Axm::as<mem::Ptr>(slot->type())->args<2>();
    auto phi    = world().proxy(T, {curr_mut(), slot}, Proxy_Phi);
    if (auto i = lattice_.find(phi); i != lattice_.end()) return i->second;
    return nullptr;
}

/*
 * Main Analysis
 */

const Def* SEO::Analysis::sccp_join(const Def* var, const Def* def) {
    DLOG("propagate called with {} and {}", var, def);

    // Pin %mem.M-typed vars to top: mem must stay threaded through every lam,
    // as later stages (clos conversion, ll backend) rely on each lam having its own mem var.
    if (Axm::isa<mem::M>(var->type())) {
        auto [i, ins] = lattice_.emplace(var, var);
        if (ins || i->second != var) {
            i->second = var;
            invalidate();
        }
        return var;
    }

    auto [i, ins] = lattice_.emplace(var, def);
    if (ins) {
        invalidate();
        DLOG("propagate: {} → {}", var, def);
        return def;
    }

    auto cur = i->second;
    if (!cur || def->isa<Bot>() || cur == def || cur == var || Proxy::isa<Proxy_GVN>(cur)) return cur;

    invalidate();
    DLOG("cannot propagate {}, trying GVN", var);
    if (cur->isa<Bot>()) return i->second = def;
    return i->second = nullptr; // we reached top for propagate; nullptr marks this to bundle for GVN
}

DefVec SEO::Analysis::sccp(Defs vars, Defs abstr_args) {
    assert(vars.size() == abstr_args.size());

    DefVec abstr_vars;
    for (size_t i = 0; i < vars.size(); i++)
        abstr_vars.emplace_back(sccp_join(vars[i], abstr_args[i]));

    return abstr_vars;
}

void SEO::Analysis::gvn_bundle(Defs vars, Defs abstr_args, Span<const Def*> abstr_vars, const Var2Idx& var2idx) {
    auto n_all = vars.size();
    for (size_t i = 0; i != n_all; ++i) {
        if (abstr_vars[i]) continue;

        auto bundle_vars = DefVec();
        bundle_vars.emplace_back(vars[i]);

        for (size_t j = i + 1; j != n_all; ++j)
            if (!abstr_vars[j] && abstr_args[j] == abstr_args[i]) bundle_vars.emplace_back(vars[j]);

        if (bundle_vars.size() == 1) {
            lattice_[vars[i]] = abstr_vars[i] = vars[i]; // top
        } else {
            auto proxy = world().proxy(vars[i]->type(), bundle_vars, Proxy_GVN);

            for (auto p : proxy->ops()) {
                auto j            = var2idx.find(p)->second;
                lattice_[vars[j]] = abstr_vars[j] = proxy;
            }

            DLOG("bundle: {}", proxy);
        }
    }
}

void SEO::Analysis::gvn_split(Defs vars,
                              Span<const Def*> abstr_args,
                              Span<const Def*> abstr_vars,
                              const Var2Idx& var2idx) {
    // E.g.: Say we started with `{a, b, c, d, e}` as a single bundle for all tvars of `lam`.
    // Now, we see `lam (x, y, x, y, z)`. Then we have to build:
    // a -> {a, c}
    // b -> {b, d}
    // c -> {a, c}
    // d -> {b, d}
    // e -> e      (top)
    for (size_t i = 0, n = vars.size(); i != n; ++i) {
        if (auto proxy = Proxy::isa<Proxy_GVN>(abstr_vars[i])) {
            auto num        = proxy->num_ops();
            auto split_vars = DefVec();

            for (auto p : proxy->ops()) {
                auto j = var2idx.find(p)->second;
                if (p == vars[j] && abstr_args[i] == abstr_args[j]) split_vars.emplace_back(vars[j]);
            }

            auto new_num = split_vars.size();
            if (new_num == 1) {
                invalidate();
                lattice_[vars[i]] = abstr_vars[i] = vars[i];
                DLOG("single: {}", vars[i]);
            } else if (new_num != num) {
                invalidate();
                auto new_proxy = world().proxy(abstr_args[i]->type(), split_vars, Proxy_GVN);
                DLOG("split: {}", new_proxy);

                for (auto p : new_proxy->ops()) {
                    auto j = var2idx.find(p)->second;
                    if (p == vars[j]) lattice_[vars[j]] = abstr_vars[j] = new_proxy;
                }
            }
            // if new_num == num: do nothing
        }
    }
}

DefVec SEO::Analysis::sccp_gvn(Defs vars, Span<const Def*> abstr_args) {
    auto abstr_vars = sccp(vars, abstr_args);

    Var2Idx var2idx;
    for (size_t i = 0; auto var : vars)
        var2idx[var] = i++;

    gvn_bundle(vars, abstr_args, abstr_vars, var2idx);
    gvn_split(vars, abstr_args, abstr_vars, var2idx);

    return abstr_vars;
}

const Def* SEO::Analysis::rewrite_imm_App(const App* app) {
    if (auto slot = Axm::isa<mem::slot>(app)) {
        if (!is_top(slot)) {
            auto [T, as]       = slot->decurry()->args<2>();
            auto [mem, id]     = slot->args<2>();
            auto [_, ptr]      = slot->projs<2>();
            auto abstr_mem     = rewrite(mem);
            auto abstr_id      = rewrite(id);
            auto sloxy         = world().proxy(ptr->type(), {curr_mut(), abstr_id}, Proxy_Slot);
            slot2type_[ptr]    = T;
            sloxy2slot_[sloxy] = slot;
            DLOG("in {}, found declaration for slot {}", curr_mut(), ptr);
            set(ptr, sloxy);
            return world().tuple({abstr_mem, sloxy});
        }
    } else if (auto store = Axm::isa<mem::store>(app)) {
        auto [mem, ptr, val] = store->args<3>();
        auto abstr_mem       = rewrite(mem);
        auto abstr_ptr       = rewrite(ptr);
        auto abstr_val       = rewrite(val);

        if (Proxy::isa<Proxy_Slot>(abstr_ptr)) {
            slot2value(abstr_ptr, abstr_val);
            DLOG("in {}, found a store: {} <- {}", curr_mut(), abstr_ptr, abstr_val);
            return abstr_mem;
        }
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, as]    = load->decurry()->args<2>();
        auto [mem, ptr] = load->args<2>();
        auto [_, val]   = load->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_ptr  = rewrite(ptr);

        if (Proxy::isa<Proxy_Slot>(abstr_ptr)) {
            if (auto abstr_val = slot2value(abstr_ptr)) {
                DLOG("abstr value: `{}`", abstr_val);
                set(val, abstr_val);
                return world().tuple({abstr_mem, abstr_val});
            }
            DLOG("couldn't resolve load of {} yet, returning bot", abstr_ptr);
            return world().tuple({abstr_mem, world().bot(T)});
        }

        DLOG("load from unknown pointer {}, treating result as top", abstr_ptr);
        return set(load, load);
    } else if (auto dispatch = Dispatch(app)) {
        // working on this alternative
        // for (size_t i = 0, e = dispatch.num_targets(); i != e; ++i)
        //     if (auto lam = dispatch.target(i)->isa_mut<Lam>()) {
        //         mut2slot2value_[curr_mut()]; // make sure no rehash happens
        //         auto& dst = mut2slot2value_[lam];
        //         dst       = mut2slot2value_[curr_mut()];
        //     }

        DefVec abstr_args;
        for (auto arg : app->targs())
            abstr_args.emplace_back(rewrite(arg));

        auto abstr_index = rewrite(dispatch.index());
        auto l           = Lit::isa(abstr_index);
        DLOG("abstract value of dispatch index {} in {}: {}", dispatch.index(), curr_mut(), abstr_index);

        for (auto [slot, _] : slot2type_) {
            auto abstr_slot = rewrite(slot);
            if (auto value = slot2value(abstr_slot)) abstr_args.emplace_back(value);
        }

        auto arms = Vector<Lam*>(dispatch.num_targets());
        for (size_t i = 0, e = arms.size(); i != e; ++i)
            arms[i] = dispatch.target(i)->isa_mut<Lam>();

        // Propagate into all arms - even if the index is abstractly known:
        // an arm skipped in one fixed-point round would accumulate a lattice - and hence a signature -
        // that differs from its siblings', but SymExprOpt::rewrite_imm_App relies on all arms agreeing.
        // For the same reason propagate either into all arms or into none.
        bool propagated = std::ranges::all_of(arms, [](Lam* lam) { return isa_optimizable(lam); });
        if (propagated) {
            for (auto lam : arms) {
                DLOG("dispatch, writing local values to {}", lam);

                DefVec vars;
                for (auto var : lam->tvars())
                    vars.emplace_back(var);

                for (auto [slot, slot_type] : slot2type_) {
                    auto abstr_slot = rewrite(slot);
                    auto phi        = world().proxy(slot_type, {lam, abstr_slot}, Proxy_Phi);
                    if (slot2value(abstr_slot)) {
                        vars.emplace_back(phi);
                    } else {
                        DLOG("no value found for {}", slot);
                        // TODO Can this ever happen?
                    }
                }

                auto abstr_vars = sccp_gvn(vars, abstr_args);
                set(lam->var(), world().tuple(abstr_vars.span().subspan(0, app->num_targs())));
                for (size_t i = 0; i < vars.size(); i++)
                    set(vars[i], abstr_vars[i]);
            }
        }

        // Propagated arms must be traversed via rewrite_deps() to keep their lattice state;
        // everything else (Var%s, externals, ...) escapes normally.
        auto rewrite_arm = [&](const Def* arm) -> const Def* {
            if (propagated) return rewrite_deps(arm->as_mut());
            return rewrite(arm);
        };

        if (l) {
            auto abstr_taken = rewrite_arm(dispatch.target(*l));
            return world().app(abstr_taken, abstr_args.span().subspan(0, app->num_targs()));
        }

        DefVec abstr_arms;
        for (size_t i = 0, e = arms.size(); i != e; ++i)
            abstr_arms.emplace_back(rewrite_arm(dispatch.target(i)));
        return world().app(world().extract(world().tuple(abstr_arms), abstr_index),
                           abstr_args.span().subspan(0, app->num_targs()));
    } else if (auto lam = app->callee()->isa_mut<Lam>(); lam && !isa_optimizable(lam)) {
        DLOG("{} not optimizable", app->callee());

        auto n          = app->num_targs();
        auto abstr_args = absl::FixedArray<const Def*>(n);

        for (size_t i = 0; i != n; ++i) {
            if (auto lam = app->targ(i)->isa_mut<Lam>(); isa_optimizable(lam)) {
                // need to rewrite this later, after slot values in the lattice are updated
                abstr_args[i] = lam;
            } else {
                DLOG("in app of non-optimizable function, rewriting arg {}", app->targ(i));
                abstr_args[i] = rewrite(app->targ(i));
            }
        }

        for (size_t i = 0; i != n; ++i) {
            if (auto lam = app->targ(i)->isa_mut<Lam>(); isa_optimizable(lam)) {
                // now propagate known slot values and rewrite the lam
                DefVec vars_inside_unknown_function;
                DefVec abstr_args_inside_unknown_function;

                // set the lams arguments to top, as we don't know what the unknown function will pass
                for (auto var : lam->tvars()) {
                    vars_inside_unknown_function.emplace_back(var);
                    abstr_args_inside_unknown_function.emplace_back(var);
                }

                // propagate known slot contents
                // some of these might escape, but we'll catch that in the postprocessing later
                for (auto [slot, slot_type] : slot2type_) {
                    auto abstr_slot = rewrite(slot);
                    auto phi        = world().proxy(slot_type, {lam, abstr_slot}, Proxy_Phi);
                    if (auto value = slot2value(abstr_slot)) {
                        vars_inside_unknown_function.emplace_back(phi);
                        abstr_args_inside_unknown_function.emplace_back(value);
                    } else {
                        DLOG("no value found for {}", slot);
                        // TODO Can this ever happen?
                        // propagate(lam_proxy, lam_proxy);
                    }
                }

                auto abstr_vars_inside_unknown_function
                    = sccp_gvn(vars_inside_unknown_function, abstr_args_inside_unknown_function);
                set(lam->var(), world().tuple(abstr_vars_inside_unknown_function.span().subspan(0, lam->num_tvars())));
                for (size_t i = 0; i < vars_inside_unknown_function.size(); i++)
                    set(vars_inside_unknown_function[i], abstr_vars_inside_unknown_function[i]);

                // now rewrite the lam
                abstr_args[i] = rewrite(app->targ(i));
            }
        }

        return world().app(rewrite_deps(lam), abstr_args);
    } else if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
        DefVec all_vars;
        DefVec all_abstr_args;

        // propagate vars
        for (size_t i = 0; i != app->num_targs(); ++i) {
            all_vars.emplace_back(lam->tvar(i));
            all_abstr_args.emplace_back(rewrite(app->targ(i)));
        }

        // propagate phis
        DLOG("propagating slot values for call of {}", lam);
        for (auto [slot, slot_type] : slot2type_) {
            DLOG("for slot {}", slot);
            auto abstr_slot = rewrite(slot);
            auto phi        = world().proxy(slot_type, {lam, abstr_slot}, Proxy_Phi);
            if (auto value = slot2value(abstr_slot)) {
                all_vars.emplace_back(phi);
                all_abstr_args.emplace_back(value);
            } else {
                DLOG("no value found for {}", slot);
                // TODO Can this ever happen?
                // propagate(lam_proxy, lam_proxy);
            }
        }

        auto all_abstr_vars = sccp_gvn(all_vars, all_abstr_args);

        set(lam->var(), world().tuple(all_abstr_vars.span().subspan(0, app->num_targs())));
        for (size_t i = app->num_targs(); i < all_vars.size(); i++)
            set(all_vars[i], all_abstr_vars[i]);

        return world().app(rewrite_deps(lam), all_abstr_args.span().subspan(0, app->num_targs()));
    }

    return Super::rewrite_imm_App(app);
}

/*
 * Post-Analysis:
 * Finds sloxies that are still present + escaping lambdas
 */

void SEO::Analysis::finalize() {
    for (auto def : world().roots())
        analyze(def);
}

void SEO::Analysis::analyze(const Def* def) {
    if (def->isa<Var>()) return;
    if (auto [_, ins] = visited_.emplace(def); !ins) return;
    if (auto l = lookup(def)) def = l;

    if (auto proxy = def->isa<Proxy>()) {
        if (proxy->tag() == Proxy_Slot) {
            auto slot     = sloxy2slot_[proxy];
            auto [_, ptr] = slot->projs<2>();
            set(slot, slot);
            set(ptr, ptr);
            DLOG("setting slot to top: {}", slot);
            invalidate();
        }
        return; // never walk a proxy's deps (would drag in meta info)
    }

    // A Lam escapes (and hence its vars must go to top) iff it is reached as a *value*.
    if (auto app = def->isa<App>()) {
        if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
            // lam is applied here, not escaped: traverse its body without seeding its vars to top
            analyze(app->arg());
            analyze(app->type());
            for (auto d : lam->deps())
                analyze(d);
            return;
        }
    } else if (auto [lam, var] = def->isa_binder<Lam>(); lam) {
        for (auto v : var->tprojs()) {
            if (auto [i, ins] = lattice_.emplace(v, v); !ins && i->second != v) {
                invalidate(); // var was mapped to sth else beforehand so we need another fixed-point round
                i->second = v;
            }
        }
    }

    for (auto d : def->deps())
        analyze(d);
}

/*
 * Transformation:
 * Apply analysis info to code
 */

static bool keep(const Def* old_var, const Def* abstr) {
    if (!abstr) return true;           // no info -> keep
    if (old_var == abstr) return true; // top
    if (auto proxy = Proxy::isa<Proxy_GVN>(abstr))
        // TODO: if old_var is a phi, only keep it if all the entries in the bundle are phis. if not, prefer the first
        // non-phi
        return proxy->op(0) == old_var; // first in GVN bundle
    else
        return false;
}

const Def* SEO::rewrite_imm_App(const App* old_app) {
    if (auto slot = Axm::isa<mem::slot>(old_app)) {
        auto [mem, id] = slot->args<2>();
        auto [_, ptr]  = slot->projs<2>();
        if (auto sloxy = lattice(ptr); sloxy && sloxy != ptr) {
            auto new_mem = rewrite(mem);
            auto new_ptr = new_world().bot(rewrite(ptr->type())); // we hopefully proved that no one uses it
            return new_world().tuple({new_mem, new_ptr});
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
            auto new_mem = rewrite(mem);
            return new_world().tuple({new_mem, rewrite(known_load)});
        }
    } else if (auto dispatch = Dispatch(old_app)) {
        auto old_arms = Vector<Lam*>(dispatch.num_targets());
        for (size_t i = 0, e = old_arms.size(); i != e; ++i)
            old_arms[i] = dispatch.target(i)->isa_mut<Lam>();

        bool all_muts = std::ranges::all_of(old_arms, [](Lam* lam) { return lam != nullptr; });
        if (all_muts && std::ranges::any_of(old_arms, [this](Lam* lam) { return needs_seo(lam); })) {
            auto new_index = rewrite(dispatch.index());
            DLOG("in {}, found dispatch on {} ({}) with {}", curr_mut(), dispatch.index(), new_index,
                 fe::Join(old_arms, " / "));

            if (auto l = Lit::isa(new_index)) {
                // the dispatch folds in the new world; only rebuild the taken arm
                auto old_taken = old_arms[*l];
                if (needs_seo(old_taken)) {
                    invalidate();
                    return map(old_app, new_world().app(build_lam(old_taken), build_args(old_taken, old_app)));
                }
                return Super::rewrite_imm_App(old_app);
            }

            invalidate();
            auto new_args = build_args(old_arms.front(), old_app);
            auto new_arms = DefVec();
            for (auto old_arm : old_arms) {
                // EtaExp runs beforehand, so all arms are fresh single-use Lams that received the same
                // propagation; hence their rebuilt signatures and arguments agree.
                assert(build_args(old_arm, old_app) == new_args);
                new_arms.emplace_back(needs_seo(old_arm) ? build_lam(old_arm) : rewrite(old_arm));
                assert(new_arms.back()->type() == new_arms.front()->type());
            }

            auto new_callee = new_world().extract(new_world().tuple(new_arms), new_index);
            return map(old_app, new_world().app(new_callee, new_args));
        }
    } else if (auto old_lam = old_app->callee()->isa_mut<Lam>(); old_lam && !isa_optimizable(old_lam)) {
        // lams passed to an unknown callee cannot receive extra args for their phis;
        // resolve each phi to the value known at this call site instead.
        for (auto old_arg : old_app->targs()) {
            auto cont = old_arg->isa_mut<Lam>();
            if (!cont) continue;

            for (auto [slot, slot_type] : analysis_.all_slots()) {
                if (auto sloxy = lattice(slot)) {
                    auto phi = old_world().proxy(slot_type, {cont, sloxy}, Proxy_Phi);
                    if (auto def = lattice(phi)) {
                        auto new_val = rewrite_site_value(sloxy, slot_type);
                        DLOG("in {}, resolving phi {} of lam {} to {}", curr_mut(), phi, cont, new_val);
                        map(phi, new_val);
                        if (def != phi && Proxy::isa<Proxy_GVN>(def)) map(def, new_val);
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

    return Super::rewrite_imm_App(old_app);
}

bool SEO::needs_seo(Lam* old_lam) {
    if (auto l = lattice(old_lam->var()); l && l != old_lam->var()) return true;

    for (auto [slot, slot_type] : analysis_.all_slots()) {
        if (auto sloxy = lattice(slot)) {
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, Proxy_Phi);
            if (auto def = lattice(phi); def && keep(phi, def)) return true;
        }
    }

    return false;
}

Lam* SEO::build_lam(Lam* old_lam) {
    if (auto i = lam2lam_.find(old_lam); i != lam2lam_.end()) return i->second;

    DLOG("building a new lam for {}", old_lam);

    // find phis
    Vector<std::pair<const Def*, const Def*>> potential_phis;
    for (auto [slot, slot_type] : analysis_.all_slots()) {
        if (auto sloxy = lattice(slot)) {
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, Proxy_Phi);
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

DefVec SEO::build_args(Lam* old_lam, const App* old_app) {
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
            auto phi = old_world().proxy(slot_type, {old_lam, sloxy}, Proxy_Phi);
            if (auto def = lattice(phi); def && keep(phi, def))
                new_args.emplace_back(rewrite_site_value(sloxy, slot_type));
        }
    }

    return new_args;
}

const Def* SEO::rewrite_site_value(const Def* sloxy, const Def* slot_type) {
    if (auto slot2value_it = analysis_.mut2slot2value().find(curr_mut());
        slot2value_it != analysis_.mut2slot2value().end()) {
        auto& slot2value = slot2value_it->second;
        if (auto found_value_it = slot2value.find(sloxy); found_value_it != slot2value.end())
            return rewrite(found_value_it->second);
    }
    // curr_mut() didn't write to the slot; forward our own phi for it
    auto curr_phi = old_world().proxy(slot_type, {curr_mut(), sloxy}, Proxy_Phi);
    return rewrite(curr_phi);
}

} // namespace mim::plug::mem::phase
