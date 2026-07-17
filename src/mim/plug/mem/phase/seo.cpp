#include "mim/plug/mem/phase/seo.h"

#include <absl/container/fixed_array.h>

#include <mim/lam.h>

#include "mim/util/util.h"

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

/*
 * Helpers
 */

enum {
    Proxy_SCCP_Top, // proxy(var)                        <- var reached ⊤ for propagation but still awaits GVN bundling
    Proxy_Bundle,   // proxy(lam, var1, var2, ..., varn) <- GVN congruence class
    Proxy_Sloxy,    // proxy(lam, ptr)                   <- ptr is the slot continuation's ptr var
    Proxy_Phi,      // proxy(lam, sloxy)
};

static size_t idx_of(Defs vars, const Def* p) {
    auto i = std::ranges::find(vars, p);
    assert(i != vars.end());
    return i - vars.begin();
}

static const Proxy* isa_bundle(const Def* def, Lam* lam);

void SEO::Analysis::reset() {
    Super::reset();
    visited_.clear();
    lam2sloxy2val_.clear();
    first_.clear();
}

/*
 * Main Analysis
 */

// SCCP

const Proxy* SEO::Analysis::mk_sccp_top(const Def* var) { return world().proxy(var->type(), {var}, Proxy_SCCP_Top); }

const Def* SEO::Analysis::sccp_join(Lam* lam, const Def* var, const Def* def) {
    DLOG("sccp_join({}, {})", var, def);
    if (is_top(var)) return var;

    // Pin %mem.M-typed vars to top: mem must stay threaded through every lam,
    // as later stages (clos conversion, ll backend) rely on each lam having its own mem var.
    if (Axm::isa<mem::M>(var->type())) return pin(var), var;

    // `⊥ ⊔ x` is `x`, but unusable if lam nests it.
    if (!def->isa<Proxy>() && lam->nests(def)) {
        DLOG("cannot propagate {} -> {}: out of scope", var, def);
        return pin(var), var;
    }

    auto cur = lattice(var);

    // Frozen: a ⊤ / this-lam's bundle wins and is kept across rounds (needs cur to exist).
    if (cur && Proxy::isa<Proxy_SCCP_Top>(cur)) return cur;
    if (cur && isa_bundle(cur, lam)) return cur;

    // First touch of `var` this round - including the very first ever (cur == ⊥):
    // restart the join from this call site, discarding any value accumulated in an earlier round.
    // The `⊥` case MUST also mark first_;
    // otherwise the *second* site would take this branch, resetting away this first contribution and
    // letting `var` settle on a later site's value instead of climbing to ⊤.
    if (auto [_, ins] = first_.emplace(var); ins) {
        DLOG("first; restart: {} -> {}", var, def);
        lattice_force(var, def); // may descend from an earlier round's ⊤ - hence force, not lattice()
        return def;
    }

    // Every first_ insertion also writes the lattice, so past the first touch `cur` exists;
    // and it cannot be ⊤, as is_top() bailed out on entry.
    assert(cur && cur != var);
    if (def->isa<Bot>() || cur == def) return cur;      // cur ⊔ ⊥ = cur; def ⊔ def = def
    if (cur->isa<Bot>()) return lattice(var, def), def; // ⊥ ⊔ def = def

    DLOG("cannot propagate {} -> {}; cur: {}, trying GVN", var, def, cur);
    auto top = mk_sccp_top(var);
    return lattice(var, top), top;
}

// GVN

static const Proxy* isa_bundle(const Def* def, Lam* lam) {
    if (auto bundle = Proxy::isa<Proxy_Bundle>(def); bundle && bundle->op(0) == lam) return bundle;
    return nullptr;
}

const Proxy* SEO::Analysis::mk_bundle(Lam* lam, const Def* var, Defs bundle_vars) {
    return world().proxy(var->type(), cat(lam, bundle_vars), Proxy_Bundle)->set(var->dbg());
}

void SEO::Analysis::gvn_bundle(Lam* lam, Defs vars, Defs abstr_args, Span<const Def*> abstr_vars) {
    auto n_all = vars.size();
    for (size_t i = 0; i != n_all; ++i) {
        if (!Proxy::isa<Proxy_SCCP_Top>(abstr_vars[i])) continue;

        auto bundle_vars = DefVec();
        bundle_vars.emplace_back(vars[i]);

        for (size_t j = i + 1; j != n_all; ++j)
            if (Proxy::isa<Proxy_SCCP_Top>(abstr_vars[j]) && abstr_args[j] == abstr_args[i])
                bundle_vars.emplace_back(vars[j]);

        if (bundle_vars.size() == 1) {
            pin(vars[i]);
            abstr_vars[i] = vars[i];
        } else {
            auto bundle = mk_bundle(lam, vars[i], bundle_vars);

            for (auto p : bundle->ops().subspan(1)) {
                auto j = idx_of(vars, p);
                lattice(vars[j], abstr_vars[j] = bundle);
            }

            DLOG("bundle: {}", bundle);
        }
    }
}

void SEO::Analysis::gvn_split(Lam* lam, Defs vars, Span<const Def*> abstr_args, Span<const Def*> abstr_vars) {
    // E.g.: Say we started with `{a, b, c, d, e}` as a single bundle for all tvars of `lam`.
    // Now, we see `lam (x, y, x, y, z)`. Then we have to build:
    // a -> {a, c}
    // b -> {b, d}
    // c -> {a, c}
    // d -> {b, d}
    // e -> e      (top)
    for (size_t i = 0, n = vars.size(); i != n; ++i) {
        if (auto bundle = isa_bundle(abstr_vars[i], lam)) {
            auto num        = bundle->num_ops() - 1;
            auto split_vars = DefVec();

            for (auto p : bundle->ops().subspan(1)) {
                auto j = idx_of(vars, p);
                if (p == vars[j] && abstr_args[i] == abstr_args[j]) split_vars.emplace_back(vars[j]);
            }

            auto new_num = split_vars.size();
            if (new_num == 1) {
                pin(vars[i]);
                abstr_vars[i] = vars[i];
                DLOG("gvn single: {}", vars[i]);
            } else if (new_num != num) {
                auto new_proxy = mk_bundle(lam, abstr_args[i], split_vars);
                DLOG("gvn split: {}", new_proxy);

                for (auto p : new_proxy->ops().subspan(1)) {
                    auto j = idx_of(vars, p);
                    if (p == vars[j]) lattice(vars[j], abstr_vars[j] = new_proxy);
                }
            }
            // if new_num == num: do nothing
        }
    }
}

// SSA

static const Def* mk_phi(World& w, Lam* lam, const Def* sloxy) {
    return w.proxy(pointee(sloxy), {lam, sloxy}, Proxy_Phi)->set(sloxy->dbg());
}

const Def* SEO::Analysis::lam2sloxy2val(Lam* lam, const Def* sloxy) {
    const auto& sloxy2val = lam2sloxy2val_[lam];
    if (auto val = mim::lookup(sloxy2val, sloxy)) return val;

    auto phi = mk_phi(world(), lam, sloxy);
    DLOG("sloxy {} not found in sloxy2val map; use phi {}", sloxy, phi);
    if (auto val = lattice(phi); val && !Proxy::isa<Proxy_SCCP_Top>(val)) return val;
    return nullptr;
}

void SEO::Analysis::propagate_phis(Lam* lam, DefVec& phis, DefVec& abstr_args) {
    for (auto ptr : slots()) {
        if (auto sloxy = Proxy::isa<Proxy_Sloxy>(rewrite(ptr))) {
            if (auto value = sloxy2val(sloxy)) {
                auto phi = mk_phi(world(), lam, sloxy);
                phis.emplace_back(phi);
                abstr_args.emplace_back(value);
                DLOG("propagate phi {} for slot {} w/ val {}", phi, sloxy, value);
            } else {
                DLOG("no value found for {}", sloxy);
            }
        }
    }
}

static void find_unknowns(DefSet& visited, LamSet& res, const Def* def) {
    if (def->isa<Proxy>()) return;
    if (auto [_, ins] = visited.emplace(def); !ins) return;

    if (auto lam = def->isa_mut<Lam>()) {
        if (lam->is_open()) res.emplace(lam);
        return;
    }

    if (def->isa_mut()) return;

    for (auto d : def->deps())
        find_unknowns(visited, res, d);
}

static void find_unknowns_callee(DefSet& visited, LamSet& res, const Def* def) {
    if (def->isa<Lam>()) return;
    find_unknowns(visited, res, def);
}

// Analysis - Rewrite

const Def* SEO::Analysis::apply_known(Lam* known, Defs abstr_targs) {
    auto n = abstr_targs.size();
    assert(n == known->num_tvars());
    DLOG("known edge: {} -> {}", curr_mut(), known);
    rewrite(known); // enqueue so its body is drained this round; a no-op if already scheduled

    DefVec all_vars(n, [&](size_t i) { return known->tvar(i); });
    DefVec all_abstr_args(abstr_targs.begin(), abstr_targs.end());

    propagate_phis(known, all_vars, all_abstr_args);

    auto all_abstr_vars = DefVec(all_vars.size());
    for (size_t i = 0, e = all_vars.size(); i != e; ++i)
        all_abstr_vars[i] = sccp_join(known, all_vars[i], all_abstr_args[i]);

    gvn_bundle(known, all_vars, all_abstr_args, all_abstr_vars);
    gvn_split(known, all_vars, all_abstr_args, all_abstr_vars);

    lattice(known->var(), world().tuple(all_abstr_vars.span().subspan(0, n)));

    for (size_t i = n, e = all_vars.size(); i != e; ++i)
        lattice(all_vars[i], all_abstr_vars[i]);

    return world().app(known, all_abstr_args.span().subspan(0, n));
}

const Def* SEO::Analysis::rewrite_imm_App(const App* app) {
    if (auto slot = Axm::isa<mem::slot>(app)) {
        if (!is_top(slot)) {
            if (auto [mem, ret_lam, _, ptr] = split_slot(slot); ret_lam) {
                auto abstr_mem     = rewrite(mem);
                auto sloxy         = world().proxy(ptr->type(), {curr_mut(), ptr}, Proxy_Sloxy)->set(slot->dbg());
                sloxy2slot_[sloxy] = slot;
                slots_.emplace(ptr);
                DLOG("slot {} -> sloxy {}", ptr, sloxy);
                // The slot is ptr's *defining* site: mark first_ so the ⊥ joined below cannot restart it away.
                assert_emplace(first_, ptr);
                lattice(ptr, sloxy);
                // Treat the slot jump like an app of `ret_lam` so mem and existing phis flow across the edge.
                // The ptr var is defined *by* the slot, so pass ⊥ (not the sloxy) as its abstract argument:
                // this keeps the sloxy out of the abstract body, so it only survives if an unresolved
                // load/store actually references it - `lattice(ptr, sloxy)` above still drives that resolution.
                return apply_known(ret_lam, {abstr_mem, world().bot(ptr->type())});
            }
        }
    } else if (auto store = Axm::isa<mem::store>(app)) {
        auto [mem, ptr, val] = store->args<3>();
        auto abstr_mem       = rewrite(mem);
        auto abstr_ptr       = rewrite(ptr);
        auto abstr_val       = rewrite(val);

        if (auto sloxy = Proxy::isa<Proxy_Sloxy>(abstr_ptr)) {
            sloxy2val(sloxy, abstr_val);
            DLOG("store: {} <- {}", sloxy, abstr_val);
            return abstr_mem;
        }
        DLOG("store w/ unknown ptr: {} <- {}", abstr_ptr, abstr_val);
    } else if (auto load = Axm::isa<mem::load>(app)) {
        auto [mem, ptr] = load->args<2>();
        auto [_, val]   = load->projs<2>();
        auto abstr_mem  = rewrite(mem);
        auto abstr_ptr  = rewrite(ptr);

        if (auto sloxy = Proxy::isa<Proxy_Sloxy>(abstr_ptr)) {
            if (auto abstr_val = sloxy2val(sloxy)) {
                DLOG("load: {} -> {}", sloxy, abstr_val);
                lattice(val, abstr_val);
                return world().tuple({abstr_mem, abstr_val});
            }
            DLOG("load w/ unknown value: {}", sloxy);
        } else {
            DLOG("load w/ unknown ptr: {}", abstr_ptr);
        }
    } else {
        auto abstr_callee = rewrite(app->callee());
        auto abstr_arg    = rewrite(app->arg());
        auto known        = abstr_callee->isa_mut<Lam>();
        if (isa_optimizable(known)) {
            DefVec abstr_targs(app->num_targs(), [&](size_t i) { return abstr_arg->tproj(i); });
            return apply_known(known, abstr_targs);
        }

        auto phi_vars       = DefVec();
        auto phi_abstr_args = DefVec();
        DefSet visited;
        LamSet lams;
        find_unknowns_callee(visited, lams, abstr_callee);
        find_unknowns(visited, lams, abstr_arg);

        for (auto lam : lams) {
            assert(lam != known && lam->is_open());
            DLOG("unknown edge: {} -> {}", curr_mut(), lam);
            propagate_phis(lam, phi_vars, phi_abstr_args);
        }

        for (size_t i = 0, e = phi_vars.size(); i != e; ++i) {
            assert_emplace(first_, phi_vars[i]);
            lattice(phi_vars[i], phi_abstr_args[i]);
        }
    }

    return Super::rewrite_imm_App(app);
}

/*
 * Post-Analysis:
 * Finds sloxies that are still present + unknown lambdas
 */

static bool keep(Lam* lam, const Def* old_var, const Def* abstr) {
    if (!abstr) return true;                            // no info -> keep
    if (old_var == abstr) return true;                  // top
    if (Proxy::isa<Proxy_SCCP_Top>(abstr)) return true; // pending ⊤: nothing was propagated -> keep
    if (auto bundle = isa_bundle(abstr, lam)) return bundle->op(1) == old_var; // use first in GVN bundle
    return false;
}

void SEO::Analysis::finalize() {
    for (auto def : world().roots())
        analyze(def);
}

void SEO::Analysis::analyze(const Def* def) {
    if (def->isa<Var>()) return; // do not run escape analysis through a Var (would remap it via lookup)
    if (auto [_, ins] = visited_.emplace(def); !ins) return;
    if (auto l = lookup(def)) def = l; // get abstracted value of def

    if (auto proxy = def->isa<Proxy>()) {
        if (proxy->tag() == Proxy_Sloxy) {
            auto ptr  = proxy->op(1); // the continuation's slot var; see rewrite_imm_App
            auto slot = sloxy2slot_[proxy];
            assert(slot);
            pin(slot);
            pin(ptr);
            DLOG("sloxy {} survived; setting slot to top: {}", proxy, slot);
        }
        return; // never walk a proxy's deps (would drag in meta info)
    }

    // A Lam is unknown (and hence its vars must go to top) iff it is reached as a *value*.
    if (auto app = def->isa<App>()) {
        if (auto slot = Axm::isa<mem::slot>(app)) {
            // The slot jump applies its continuation, so `ret_lam` is known - not reached as a value.
            auto [mem, ret_lam, _, __] = split_slot(slot);
            analyze(app->type());
            analyze(mem); // the ptr var has no argument - the slot itself defines it
            for (auto d : ret_lam->deps())
                analyze(d);
            return;
        }
        if (auto lam = app->callee()->isa_mut<Lam>(); isa_optimizable(lam)) {
            // lam is applied here, it's known: traverse its body without pinning its vars to top
            analyze(app->type());

            // only analyze args that we keep
            for (size_t i = 0, e = lam->num_tdoms(); i != e; ++i) {
                auto old_var = lam->var(e, i);
                if (keep(lam, old_var, lattice(old_var))) analyze(app->arg(e, i));
            }

            for (auto d : lam->deps())
                analyze(d);

            return;
        }
    } else if (auto [lam, var] = def->isa_binder<Lam>(); lam) {
        DLOG("lam {} unknown", lam);
        unknowns_.emplace(lam);
        for (auto v : var->tprojs())
            pin(v);
    }

    for (auto d : def->deps())
        analyze(d);
}

/*
 * Transformation:
 * Apply analysis info to code
 */

const Def* SEO::isa_optimized_sloxy(const Def* def) const {
    if (auto l = lattice(def))
        if (auto sloxy = Proxy::isa<Proxy_Sloxy>(l)) return sloxy;
    return nullptr;
}

const Def* SEO::rewrite_imm_App(const App* old_app) {
    if (auto slot = Axm::isa<mem::slot>(old_app)) {
        auto [mem, ret_lam, _, ptr] = split_slot(slot);

        if (isa_optimized_sloxy(ptr)) {
            // The slot was promoted away: jump straight to the (rebuilt) continuation, dropping the ptr var.
            assert(!analysis_.unknowns().contains(ret_lam)); // promoted -> ret_lam was never reached as a value
            auto& phis    = phis_of(ret_lam);
            auto new_lam  = build_lam(phis, ret_lam);
            auto new_args = build_args(phis, ret_lam, {mem, ptr});
            return map(old_app, new_world().app(new_lam, new_args));
        }

        // The slot survives: keep the allocation, forwarding the continuation.
        auto [T, a]      = slot->decurry()->args<2>();
        auto new_mem     = rewrite(mem);
        auto new_ret_lam = rewrite(ret_lam)->as_mut<Lam>();
        return map(old_app, mem::op_slot(rewrite(T), rewrite(a), new_mem, new_ret_lam));
    } else if (auto store = Axm::isa<mem::store>(old_app)) {
        auto [mem, ptr, val] = store->args<3>();
        if (isa_optimized_sloxy(ptr)) return rewrite(mem);
    } else if (auto load = Axm::isa<mem::load>(old_app)) {
        auto [res_mem, res_val] = load->projs<2>();
        auto [mem, ptr]         = load->args<2>();
        if (auto sloxy = isa_optimized_sloxy(ptr)) {
            auto abstr_val = abstracted(res_val);
            assert(abstr_val && "a promoted slot implies every load from it resolved");
            DLOG("rewriting a load from {}, we know that it's {}", sloxy, abstr_val);
            auto new_mem = rewrite(mem);
            return new_world().tuple({new_mem, rewrite(abstr_val)});
        }
    } else {
        auto old_lam = old_app->callee()->isa_mut<Lam>();
        if (!old_lam) {
            // The callee may fold to a rebuilt lam in the new world only,
            // e.g. a branch `(f, t)#cond` whose cond becomes constant after GVN merged vars.
            if (auto new_lam = rewrite(old_app->callee())->isa_mut<Lam>())
                if (auto ol = mim::lookup(lam_new2old_, new_lam)) old_lam = ol;
        }

        if (old_lam) {
            DLOG("in {}, found app of {}", curr_mut(), old_lam);

            auto& phis = phis_of(old_lam);
            if (needs_seo(phis, old_lam)) {
                DLOG("needs seo: {}", old_lam);
                auto new_lam = build_lam(phis, old_lam);
                DefVec old_targs(old_lam->num_tvars(), [&](size_t i) { return old_app->targ(i); });
                auto new_args = build_args(phis, old_lam, old_targs);
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
        for (auto ptr : analysis_.slots())
            if (auto sloxy = isa_optimized_sloxy(ptr)) {
                auto phi = mk_phi(old_world(), old_lam, sloxy);
                if (auto val = lattice(phi); val && !Proxy::isa<Proxy_SCCP_Top>(val))
                    phis.emplace_back(sloxy, phi, val);
            }
    }
    return phis;
}

bool SEO::needs_seo(View<Phi> phis, Lam* old_lam) {
    // An unknown lam is used as a value somewhere; its signature must stay as is.
    if (analysis_.unknowns().contains(old_lam)) return false;

    // A signature change is needed iff some var is dropped/propagated/merged (i.e. not kept as ⊤) ...
    for (size_t i = 0, n = old_lam->num_tvars(); i != n; ++i) {
        auto old_var = old_lam->var(n, i);
        if (!keep(old_lam, old_var, lattice(old_var))) return true;
    }

    // ... or some phi has to be threaded in.
    for (auto [sloxy, phi, val] : phis)
        if (keep(old_lam, phi, val)) return true;

    return false;
}

Lam* SEO::build_lam(View<Phi> phis, Lam* old_lam) {
    if (auto new_lam = mim::lookup(lam_old2new_, old_lam)) return new_lam;

    DLOG("building a new lam for {}", old_lam);
    invalidate();
    size_t num_old = old_lam->num_tvars();

    // build new dom
    auto keeps    = absl::FixedArray<bool>(num_old);
    auto new_doms = DefVec();
    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        keeps[i]     = keep(old_lam, old_var, lattice(old_var));
        if (keeps[i]) new_doms.emplace_back(rewrite(old_lam->dom(num_old, i)));
    }

    for (auto [sloxy, phi, val] : phis)
        if (keep(old_lam, phi, val)) new_doms.emplace_back(rewrite(phi->type()));

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
            if (auto abstr = lattice(old_var))
                if (auto bundle = isa_bundle(abstr, old_lam)) map(bundle, v); // GVN bundle
        }
    }

    for (auto [sloxy, phi, val] : phis) {
        if (keep(old_lam, phi, val)) {
            auto v = new_lam->var(num_new_vars, j++);
            DLOG("mapping phi {} to {}", phi, v);
            map(phi, v);
            if (val != phi) map(val, v); // phi is part of a GVN bundle
        }
    }

    // now resolve the dropped vars to their propagated values
    for (size_t i = 0; i != num_old; ++i)
        if (!keeps[i]) {
            auto old_var = old_lam->var(num_old, i);
            auto abstr   = lattice(old_var);
            // A dropped slot ptr (a promoted stack slot) carries no value: map it to ⊥.
            auto new_def = Proxy::isa<Proxy_Sloxy>(abstr) ? new_world().bot(rewrite(old_lam->dom(num_old, i)))
                                                          : rewrite(abstr); // SCCP propagate
            DLOG("propagate: old_lam {} - new_lam {}; var {} - with {}", old_lam, new_lam, i, new_def);
            var_map[i] = new_def;
        }

    // Map the whole var *before* rewriting dropped-phi values below: such a value may itself project a
    // *dropped* var of old_lam (e.g. its now-removed empty closure env), which is only reachable through
    // var_map - not the individually-mapped kept projections. Without this the projection falls through to
    // the freshly built (narrower) var and its index no longer fits.
    map(old_lam->var(), var_map);

    for (auto [sloxy, phi, val] : phis)
        if (!keep(old_lam, phi, val)) {
            DLOG("mapping phi {} to its propagated value {}", phi, val);
            map(phi, rewrite(val));
        }

    {
        auto _          = enter(old_lam);
        auto new_filter = rewrite(old_lam->filter());
        auto new_body   = rewrite(old_lam->body());
        new_lam->set(new_filter, new_body);
    }

    return new_lam;
}

DefVec SEO::build_args(View<Phi> phis, Lam* old_lam, Defs old_targs) {
    size_t num_old = old_lam->num_tvars();
    assert(old_targs.size() == num_old);
    auto new_args = DefVec();

    for (size_t i = 0; i != num_old; ++i) {
        auto old_var = old_lam->var(num_old, i);
        auto abstr   = lattice(old_var);
        if (keep(old_lam, old_var, abstr)) new_args.emplace_back(rewrite(old_targs[i]));
    }

    DLOG("wiring up phi arguments");
    for (auto [sloxy, phi, val] : phis)
        if (keep(old_lam, phi, val)) {
            auto arg = analysis_.lam2sloxy2val(curr_mut<Lam>(), sloxy);
            assert(arg);
            new_args.emplace_back(rewrite(arg));
        }

    return new_args;
}

} // namespace mim::plug::mem::phase
