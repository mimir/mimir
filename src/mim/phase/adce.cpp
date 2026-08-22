#include "mim/phase/adce.h"

namespace mim {

enum { Proxy_Dead; };

const Proxy* ADCE::Analysis::mk_proxy(const Def* var) { return world().proxy(var->type(), {var}, Proxy_Dead); }

const Def* ADCE::rewrite_mut_Lam(Lam* old_lam) {
    if (!is_bootstrapping()) {
        if (auto statics = this->statics(old_lam); !statics.empty()) {
            auto& w        = new_world();
            auto n         = statics.size();
            auto loop_doms = DefVec();
            for (size_t i = 0; i != n; ++i)
                if (!statics[i]) loop_doms.emplace_back(rewrite(old_lam->tdom(i)));

            auto wrap = w.mut_lam(rewrite(old_lam->type())->as<Pi>())->set(old_lam->dbg_key());
            auto loop = w.mut_lam(loop_doms, rewrite(old_lam->codom()))->set(old_lam->dbg_key());
            loop->debug_suffix("_loop");
            DLOG("old {} -> (wrap: {}, loop: {})", old_lam, wrap, loop);
            old2wrap_loop_[old_lam] = {wrap, loop};

            // The body lives in loop; the static vars stay wrap's and are free in loop.
            DefVec vars(n), args;
            for (size_t i = 0, j = 0; i != n; ++i) {
                if (statics[i]) {
                    vars[i] = wrap->tvar(i);
                } else {
                    vars[i] = loop->var(loop_doms.size(), j++);
                    args.emplace_back(wrap->tvar(i));
                }
            }

            map(old_lam, wrap);
            map(old_lam->var(), vars);
            loop->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
            wrap->app(false, loop, args);
            return wrap;
        }
    }

    return RWPhase::rewrite_mut_Lam(old_lam);
}
/*
 * Post-Analysis:
 * Finds sloxies that are still present + unknown lambdas
 */

void ACDE::Analysis::finalize() {
    for (auto def : world().roots())
        analyze(def);
}

void ACDE::Analysis::analyze(const Def* def) {
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

const Def* ADCE::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>(); old_lam && !is_bootstrapping()) {
        if (auto statics = this->statics(old_lam); !statics.empty()) {
            rewrite(old_lam); // make sure wrap/loop exist
            if (auto i = old2wrap_loop_.find(old_lam); i != old2wrap_loop_.end()) {
                auto loop = i->second.second;
                auto n    = statics.size();
                auto args = DefVec();
                for (size_t i = 0; i != n; ++i) {
                    auto old_arg = old_app->targ(i);
                    if (!statics[i])
                        args.emplace_back(rewrite(old_arg));
                    else if (old_arg != old_lam->tvar(i))
                        return RWPhase::rewrite_imm_App(old_app); // not our class: go through wrap
                }
                invalidate();
                return new_world().app(loop, args);
            }
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim
