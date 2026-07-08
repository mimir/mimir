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

static size_t idx_of(Defs vars, const Def* p) {
    auto it = std::ranges::find(vars, p);
    assert(it != vars.end());
    return it - vars.begin();
}

void SEO::Analysis::reset() {
    Super::reset();
    visited_.clear();
    mut2sloxy2val_.clear();
}

/*
 * Main Analysis
 */

// SCCP

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

// GVN

void SEO::Analysis::gvn_bundle(Defs vars, Defs abstr_args, Span<const Def*> abstr_vars) {
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
                auto j            = idx_of(vars, p);
                lattice_[vars[j]] = abstr_vars[j] = proxy;
            }

            DLOG("bundle: {}", proxy);
        }
    }
}

void SEO::Analysis::gvn_split(Defs vars, Span<const Def*> abstr_args, Span<const Def*> abstr_vars) {
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
                auto j = idx_of(vars, p);
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
                    auto j = idx_of(vars, p);
                    if (p == vars[j]) lattice_[vars[j]] = abstr_vars[j] = new_proxy;
                }
            }
            // if new_num == num: do nothing
        }
    }
}

// SSA

static const Def* mk_phi(World& w, Lam* lam, const Def* sloxy) {
    return w.proxy(pointee(sloxy), {lam, sloxy}, Proxy_Phi);
}

const Def* SEO::Analysis::sloxy2val(const Def* sloxy) {
    const auto& sloxy2val = mut2sloxy2val_[curr_mut()];
    if (auto i = sloxy2val.find(sloxy); i != sloxy2val.end()) return i->second;

    // not in the local map: check if we have a phi for this slot in the lattice
    auto phi = mk_phi(world(), curr_mut<Lam>(), sloxy);
    if (auto i = lattice_.find(phi); i != lattice_.end()) return i->second;
    return nullptr;
}

void SEO::Analysis::propagate_phis(Lam* lam, DefVec& vars, DefVec& abstr_args) {
    DLOG("propagating slot values for call of {}", lam);
    for (auto slot : slots()) {
        DLOG("for slot {}", slot);
        auto abstr_slot = rewrite(slot);
        if (auto value = sloxy2val(abstr_slot)) {
            auto phi = mk_phi(world(), lam, abstr_slot);
            vars.emplace_back(phi);
            abstr_args.emplace_back(value);
        } else {
            DLOG("no value found for {}", slot);
        }
    }
}

Vector<SEO::Phi> SEO::phis(Lam* old_lam) {
    Vector<Phi> result;
    for (auto slot : analysis_.slots())
        if (auto sloxy = lattice(slot)) {
            auto phi = mk_phi(old_world(), old_lam, sloxy);
            if (auto val = lattice(phi)) result.emplace_back(sloxy, phi, val);
        }
    return result;
}

// Analysis - Rewrite

const Def* SEO::Analysis::rewrite_imm_App(const App* app) {
    if (auto slot = Axm::isa<mem::slot>(app)) {
        if (!is_top(slot)) {
            auto [mem, id] = slot->args<2>();
            auto [_, ptr]  = slot->projs<2>();
            auto abstr_mem = rewrite(mem);
            auto abstr_id  = rewrite(id);
            auto sloxy     = world().proxy(ptr->type(), {curr_mut(), abstr_id}, Proxy_Slot);
            slots_.emplace(ptr);
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

        if (auto sloxy = Proxy::isa<Proxy_Slot>(abstr_ptr)) {
            sloxy2val(sloxy, abstr_val);
            DLOG("in {}, found a store: {} <- {}", curr_mut(), sloxy, abstr_val);
            return abstr_mem;
        }
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, as]    = load->decurry()->args<2>();
        auto [mem, ptr] = load->args<2>();
        auto [_, val]   = load->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_ptr  = rewrite(ptr);

        if (auto sloxy = Proxy::isa<Proxy_Slot>(abstr_ptr)) {
            if (auto abstr_val = sloxy2val(sloxy)) {
                DLOG("abstr value: `{}`", abstr_val);
                set(val, abstr_val);
                return world().tuple({abstr_mem, abstr_val});
            }
            DLOG("couldn't resolve load of {} yet, returning bot", sloxy);
            return world().tuple({abstr_mem, world().bot(T)});
        }

        DLOG("load from unknown pointer {}, treating result as top", abstr_ptr);
        return set(load, load);
    } else {
        auto abstr_callee = rewrite(app->callee());
        auto abstr_arg    = rewrite(app->arg());
        auto known        = abstr_callee->isa_mut<Lam>();
        if (isa_optimizable(known)) {
            DefVec all_vars;
            DefVec all_abstr_args;

            // propagate vars
            for (size_t i = 0; i != app->num_targs(); ++i) {
                all_vars.emplace_back(known->tvar(i));
                all_abstr_args.emplace_back(abstr_arg->tproj(i));
            }

            propagate_phis(known, all_vars, all_abstr_args);

            auto all_abstr_vars = sccp(all_vars, all_abstr_args);
            gvn_bundle(all_vars, all_abstr_args, all_abstr_vars);
            gvn_split(all_vars, all_abstr_args, all_abstr_vars);

            set(known->var(), world().tuple(all_abstr_vars.span().subspan(0, app->num_targs())));

            for (size_t i = app->num_targs(), e = all_vars.size(); i != e; ++i)
                set(all_vars[i], all_abstr_vars[i]);

            return world().app(rewrite(known), all_abstr_args.span().subspan(0, app->num_targs()));
        }

        DefVec phi_vars;
        DefVec phi_abstr_args;
        auto all_muts = world().muts().merge(abstr_callee->local_muts(), abstr_arg->local_muts());
        for (auto mut : all_muts) {
            if (auto lam = mut->isa<Lam>()) {
                if (lam == known || lam->is_closed()) continue;
                propagate_phis(lam, phi_vars, phi_abstr_args);
            }
        }

        for (size_t i = 0, e = phi_vars.size(); i != e; ++i)
            sccp_join(phi_vars[i], phi_abstr_args[i]);
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
            analyze(app->type());
            analyze(app->arg());
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
        if (abstracted(ptr)) {
            auto new_mem = rewrite(mem);
            auto new_ptr = new_world().bot(rewrite(ptr->type())); // we hopefully proved that no one uses it
            return new_world().tuple({new_mem, new_ptr});
        }
    } else if (auto store = Axm::isa<mem::store>(old_app)) {
        auto [mem, ptr, val] = store->args<3>();
        if (abstracted(ptr)) return rewrite(mem);
    } else if (auto load = Axm::isa<mem::load>(old_app)) {
        auto [res_mem, res_val] = load->projs<2>();
        auto [mem, ptr]         = load->args<2>();
        DLOG("rewriting a load from {} ({})", ptr, old_app);
        if (auto abstr_val = abstracted(res_val)) {
            DLOG("rewriting a load from {}, we know that it's {}", ptr, abstr_val);
            auto new_mem = rewrite(mem);
            return new_world().tuple({new_mem, rewrite(abstr_val)});
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
    if (abstracted(old_lam->var())) return true;

    for (auto [sloxy, phi, val] : phis(old_lam))
        if (keep(phi, val)) return true;

    return false;
}

Lam* SEO::build_lam(Lam* old_lam) {
    if (auto i = lam2lam_.find(old_lam); i != lam2lam_.end()) return i->second;

    DLOG("building a new lam for {}", old_lam);

    // find phis
    auto potential_phis = phis(old_lam);

    size_t num_old = old_lam->num_tvars();

    // build new dom
    auto new_doms = DefVec();
    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);
        if (keep(old_var, abstr)) new_doms.emplace_back(rewrite(old_lam->dom(num_old, i)));
    }

    for (auto [sloxy, phi, val] : potential_phis)
        if (keep(phi, val)) new_doms.emplace_back(rewrite(phi->type()));

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

    for (auto [sloxy, phi, val] : potential_phis) {
        DLOG("deciding if we keep the phi: {}, {}", phi, val);
        if (keep(phi, val)) {
            auto v = new_lam->var(num_new_vars, j++);
            DLOG("mapping phi {} to {}", phi, v);
            map(phi, v);
            if (val != phi) {
                DLOG("need to map a phi to gvn bundle");
                map(val, v);
            }
        } else {
            DLOG("need to map a phi to constant value");
            map(phi, rewrite(val));
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
    for (auto [sloxy, phi, val] : phis(old_lam))
        if (keep(phi, val)) new_args.emplace_back(rewrite_site_value(sloxy));

    return new_args;
}

const Def* SEO::rewrite_site_value(const Def* sloxy) {
    if (auto slot2value_it = analysis_.mut2sloxy2val().find(curr_mut());
        slot2value_it != analysis_.mut2sloxy2val().end()) {
        auto& sloxy2val = slot2value_it->second;
        if (auto found_value_it = sloxy2val.find(sloxy); found_value_it != sloxy2val.end())
            return rewrite(found_value_it->second);
    }
    // curr_mut() didn't write to the slot; forward our own phi for it
    return rewrite(mk_phi(old_world(), curr_mut<Lam>(), sloxy));
}

} // namespace mim::plug::mem::phase
