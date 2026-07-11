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

/// The Lam the abstract @p var belongs to; @p var is a Var, a Var projection, or a phi Proxy.
static Lam* lam_of(const Def* var) {
    if (auto proxy = var->isa<Proxy>()) return proxy->op(0)->as_mut<Lam>();
    if (auto ex = var->isa<Extract>()) return ex->tuple()->as<Var>()->mut()->as_mut<Lam>();
    return var->as<Var>()->mut()->as_mut<Lam>();
}

/// May @p def be substituted for a var of @p lam inside @p lam's body?
/// A free var of @p def that is @p lam's own var or already free in @p lam is trivially fine.
/// Any other free var effectively moves @p lam underneath its binder,
/// so it must be visible at every use of @p lam;
/// otherwise the substitution would introduce an unbound var there.
/// A user that references @p lam's own var sits inside @p lam's scope and relocates with it - skip those.
static bool visible(const Def* def, Lam* lam) {
    auto lam_var = lam->has_var();
    for (auto fv : def->free_vars()) {
        if (fv == lam_var || lam->free_vars().contains(fv)) continue;
        for (auto user : lam->users()) {
            if (lam_var && user->free_vars().contains(lam_var)) continue;
            if (fv != user->has_var() && !user->free_vars().contains(fv)) return false;
        }
    }
    return true;
}

void SEO::Analysis::prepare() {
    // make sure def->users() are valid
    for (auto def : world().roots())
        def->free_vars();
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
    DLOG("sccp_join({}, {})", var, def);

    // Pin %mem.M-typed vars to top: mem must stay threaded through every lam,
    // as later stages (clos conversion, ll backend) rely on each lam having its own mem var.
    if (Axm::isa<mem::M>(var->type())) return pin_top(var);

    // A value can only be propagated if it is visible at all uses of the var's lam;
    // e.g. `⊥ ⊔ x` is `x`, but unusable if some use of the lam sits outside `x`'s scope.
    if (!def->isa<Proxy>() && !def->free_vars().empty() && !visible(def, lam_of(var))) {
        DLOG("cannot propagate {} -> {}: out of scope", var, def);
        return pin_top(var);
    }

    auto cur = lattice(var);
    if (!cur) {
        if (lattice().contains(var)) return nullptr; // nullptr sentinel: already marked to bundle for GVN

        update(var, def); // ⊥ ⊔ def = def; update() invalidates, as it inserts a fresh fact
        DLOG("propagate: {} -> {}", var, def);
        return def;
    }

    if (def->isa<Bot>() || cur == def || cur == var || Proxy::isa<Proxy_GVN>(cur)) return cur;

    DLOG("cannot propagate {} -> {}, trying GVN", var, def);
    // update() invalidates, as it overwrites cur in both cases.
    if (cur->isa<Bot>()) { // Bot ⊔ def = def
        update(var, def);
        return def;
    }
    update(var, nullptr); // we reached top for propagate; nullptr marks this to bundle for GVN
    return nullptr;
}

DefVec SEO::Analysis::sccp(Defs vars, Defs abstr_args) {
    assert(vars.size() == abstr_args.size());

    DefVec abstr_vars;
    for (size_t i = 0; i < vars.size(); i++)
        abstr_vars.emplace_back(sccp_join(vars[i], abstr_args[i]));

    return abstr_vars;
}

// GVN

const Proxy* SEO::Analysis::mk_bundle(const Def* var, Defs bundle_vars) {
    return world().proxy(var->type(), bundle_vars, Proxy_GVN);
}

void SEO::Analysis::gvn_bundle(Defs vars, Defs abstr_args, Span<const Def*> abstr_vars) {
    auto n_all = vars.size();
    for (size_t i = 0; i != n_all; ++i) {
        if (abstr_vars[i]) continue;

        auto bundle_vars = DefVec();
        bundle_vars.emplace_back(vars[i]);

        for (size_t j = i + 1; j != n_all; ++j)
            if (!abstr_vars[j] && abstr_args[j] == abstr_args[i]) bundle_vars.emplace_back(vars[j]);

        if (bundle_vars.size() == 1) {
            abstr_vars[i] = pin_top(vars[i]);
        } else {
            auto bundle = mk_bundle(vars[i], bundle_vars);

            for (auto p : bundle->ops()) {
                auto j = idx_of(vars, p);
                update(vars[j], abstr_vars[j] = bundle);
            }

            DLOG("bundle: {}", bundle);
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
                abstr_vars[i] = pin_top(vars[i]);
                DLOG("single: {}", vars[i]);
            } else if (new_num != num) {
                auto new_proxy = mk_bundle(abstr_args[i], split_vars);
                DLOG("split: {}", new_proxy);

                for (auto p : new_proxy->ops()) {
                    auto j = idx_of(vars, p);
                    if (p == vars[j]) update(vars[j], abstr_vars[j] = new_proxy);
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

const Def* SEO::Analysis::mut2sloxy2val(Lam* lam, const Def* sloxy) {
    const auto& sloxy2val = mut2sloxy2val_[lam];
    if (auto i = sloxy2val.find(sloxy); i != sloxy2val.end()) return i->second;

    auto phi = mk_phi(world(), lam, sloxy);
    DLOG("sloxy {} not found in sloxy2val map; use phi {}", sloxy, phi);
    return lattice(phi);
}

void SEO::Analysis::propagate_phis(Lam* lam, DefVec& phis, DefVec& abstr_args) {
    for (auto slot : slots()) {
        auto abstr_slot = rewrite(slot);
        if (auto value = sloxy2val(abstr_slot)) {
            auto phi = mk_phi(world(), lam, abstr_slot);
            phis.emplace_back(phi);
            abstr_args.emplace_back(value);
            DLOG("propgate phi {} for slot {} w/ val {}", phi, abstr_slot, value);
        } else {
            DLOG("no value found for {}", slot);
        }
    }
}

// Analysis - Rewrite

const Def* SEO::Analysis::rewrite_imm_App(const App* app) {
    if (auto slot = Axm::isa<mem::slot>(app)) {
        if (!is_top(slot)) {
            auto [mem, id]     = slot->args<2>();
            auto [_, ptr]      = slot->projs<2>();
            auto abstr_mem     = rewrite(mem);
            auto abstr_id      = rewrite(id);
            auto sloxy         = world().proxy(ptr->type(), {curr_mut(), abstr_id}, Proxy_Slot);
            sloxy2slot_[sloxy] = slot;
            slots_.emplace(ptr);
            DLOG("slot {} -> sloxy {}", ptr, sloxy);
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
            DLOG("store: {} <- {}", sloxy, abstr_val);
            return abstr_mem;
        }
        DLOG("store w/ unknown ptr: {} <- {}", abstr_ptr, abstr_val);
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, as]    = load->decurry()->args<2>();
        auto [mem, ptr] = load->args<2>();
        auto [_, val]   = load->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_ptr  = rewrite(ptr);

        if (auto sloxy = Proxy::isa<Proxy_Slot>(abstr_ptr)) {
            if (auto abstr_val = sloxy2val(sloxy)) {
                DLOG("load: {} -> {}", sloxy, abstr_val);
                set(val, abstr_val);
                return world().tuple({abstr_mem, abstr_val});
            }
            DLOG("load w/ unknown value: {}", sloxy);
            return world().tuple({abstr_mem, world().bot(T)});
        }

        DLOG("load w/ unknown ptr: {}", abstr_ptr);
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

            return world().app(known, all_abstr_args.span().subspan(0, app->num_targs()));
        }

        auto phi_vars       = DefVec();
        auto phi_abstr_args = DefVec();
        auto all_muts       = world().muts().merge(abstr_callee->local_muts(), abstr_arg->local_muts());
        for (auto mut : all_muts) {
            if (auto lam = mut->isa<Lam>()) {
                if (lam == known || lam->is_closed()) continue;
                DLOG("unknown edge: {} -> {}", curr_mut(), lam);
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
            auto slot = sloxy2slot_[proxy];
            assert(slot);
            auto [_, ptr] = slot->projs<2>();
            set(slot, slot);
            set(ptr, ptr);
            DLOG("sloxy {} survived; setting slot to top: {}", def, slot);
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
        DLOG("lam {} escapes", lam);
        escaped_.emplace(lam);
        for (auto v : var->tprojs())
            if (auto old = update(v, v); old && old != v) DLOG("top: {}", v);
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
    } else {
        auto old_lam = old_app->callee()->isa_mut<Lam>();
        if (!old_lam) {
            // The callee may fold to a rebuilt lam in the new world only, e.g. a branch
            // `(f, t)#cond` whose cond becomes constant after GVN merged vars.
            if (auto new_lam = rewrite(old_app->callee())->isa_mut<Lam>())
                if (auto i = lam_new2old_.find(new_lam); i != lam_new2old_.end()) old_lam = i->second;
        }

        if (old_lam) {
            DLOG("in {}, found app of {}", curr_mut(), old_lam);

            auto& phis = phis_of(old_lam);
            if (needs_seo(phis, old_lam)) {
                auto new_lam  = build_lam(phis, old_lam);
                auto new_args = build_args(phis, old_lam, old_app);
                return map(old_app, new_world().app(new_lam, new_args));
            }
        }
    }

    return Super::rewrite_imm_App(old_app);
}

const Def* SEO::rewrite_mut_Lam(Lam* old_lam) {
    // A lam that gets a new signature must never be rebuilt generically:
    // otherwise two new versions of old_lam exist and their (hash-consed, cached) body defs
    // reference whichever version was rewritten first - leaving free vars in the other one.
    if (auto& phis = phis_of(old_lam); needs_seo(phis, old_lam)) return build_lam(phis, old_lam);
    return Super::rewrite_mut_Lam(old_lam);
}

const Vector<SEO::Phi>& SEO::phis_of(Lam* old_lam) {
    auto [i, ins] = lam2phis_.emplace(old_lam, Vector<Phi>());
    auto& phis    = i->second;
    if (ins) {
        for (auto slot : analysis_.slots())
            if (auto sloxy = lattice(slot)) {
                auto phi = mk_phi(old_world(), old_lam, sloxy);
                if (auto val = lattice(phi)) phis.emplace_back(sloxy, phi, val);
            }
    }
    return phis;
}

bool SEO::needs_seo(View<Phi> phis, Lam* old_lam) {
    // An escaped lam is used as a value somewhere; its signature must stay as is.
    if (analysis_.escaped().contains(old_lam)) return false;

    if (abstracted(old_lam->var())) return true;

    for (auto [sloxy, phi, val] : phis)
        if (keep(phi, val)) return true;

    return false;
}

Lam* SEO::build_lam(View<Phi> phis, Lam* old_lam) {
    if (auto i = lam_old2new_.find(old_lam); i != lam_old2new_.end()) return i->second;

    DLOG("building a new lam for {}", old_lam);
    invalidate();
    size_t num_old = old_lam->num_tvars();

    // build new dom
    auto keeps    = absl::FixedArray<bool>(num_old);
    auto new_doms = DefVec();
    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        keeps[i]     = keep(old_var, lattice(old_var));
        if (keeps[i]) new_doms.emplace_back(rewrite(old_lam->dom(num_old, i)));
    }

    for (auto [sloxy, phi, val] : phis)
        if (keep(phi, val)) new_doms.emplace_back(rewrite(phi->type()));

    size_t num_new_vars = new_doms.size();

    // build new lam
    auto var_map          = absl::FixedArray<const Def*>(num_old);
    auto new_lam          = new_world().mut_lam(new_doms, rewrite(old_lam->codom()))->set(old_lam->dbg());
    lam_old2new_[old_lam] = new_lam;
    lam_new2old_[new_lam] = old_lam;

    // Map all *kept* vars/phis before rewriting any propagated value below:
    // that rewrite may recursively re-enter old_lam (via apps of it in other muts) and
    // must be able to resolve the kept projections - otherwise the rewriter falls back
    // to a second, generic stub of old_lam whose cached body defs poison ours.
    size_t j = 0;

    for (size_t i = 0; i != num_old; ++i) {
        if (keeps[i]) {
            auto old_var = old_lam->var(num_old, i);
            auto v       = new_lam->var(num_new_vars, j++)->set(old_var->dbg());
            var_map[i]   = map(old_var, v);
            if (auto abstr = lattice(old_var); abstr && abstr != old_var) map(abstr, v); // GVN bundle
        }
    }

    for (auto [sloxy, phi, val] : phis) {
        if (keep(phi, val)) {
            auto v = new_lam->var(num_new_vars, j++);
            DLOG("mapping phi {} to {}", phi, v);
            map(phi, v);
            if (val != phi) map(val, v); // phi is part of a GVN bundle
        }
    }

    // now resolve the dropped vars/phis to their propagated values
    for (size_t i = 0; i != num_old; ++i)
        if (!keeps[i]) var_map[i] = rewrite(lattice(old_lam->var(num_old, i))); // SCCP propagate

    for (auto [sloxy, phi, val] : phis)
        if (!keep(phi, val)) {
            DLOG("mapping phi {} to its propagated value {}", phi, val);
            map(phi, rewrite(val));
        }

    map(old_lam->var(), var_map);
    {
        auto _          = enter(old_lam);
        auto new_filter = rewrite(old_lam->filter());
        auto new_body   = rewrite(old_lam->body());
        new_lam->set(new_filter, new_body);
    }

    return new_lam;
}

DefVec SEO::build_args(View<Phi> phis, Lam* old_lam, const App* old_app) {
    size_t num_old = old_lam->num_tvars();
    auto new_args  = DefVec();

    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);
        if (keep(old_var, abstr)) new_args.emplace_back(rewrite(old_app->targ(i)));
    }

    DLOG("wiring up phi arguments");
    for (auto [sloxy, phi, val] : phis)
        if (keep(phi, val)) {
            auto arg = analysis_.mut2sloxy2val(curr_mut<Lam>(), sloxy);
            assert(arg);
            new_args.emplace_back(rewrite(arg));
        }

    return new_args;
}

} // namespace mim::plug::mem::phase
