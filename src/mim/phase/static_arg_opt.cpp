#include "mim/phase/static_arg_opt.h"

namespace mim {

bool StaticArgOpt::analyze() {
    for (auto def : old_world().roots())
        analyze(def);
    return false; // no fixed-point necessary
}

void StaticArgOpt::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    if (auto app = def->isa<App>())
        if (auto lam = app->callee()->isa_mut<Lam>(); lam && lam->has_var()) visit(app, lam);

    for (auto d : def->deps())
        analyze(d);
}

void StaticArgOpt::visit(const App* app, Lam* lam) {
    auto n    = lam->num_tdoms();
    auto mask = Mask(n, false);
    auto self = false;
    for (size_t i = 0; i != n; ++i)
        self |= (mask[i] = app->targ(i) == lam->tvar(i));
    if (self) lam2sites_[lam].emplace_back(std::move(mask)); // only lam's own body can mention lam's vars
}

StaticArgOpt::Mask StaticArgOpt::statics(Lam* lam) {
    if (auto i = lam2statics_.find(lam); i != lam2statics_.end()) return i->second;

    // A *mutable* Pi is a dependent one; splitting it would require loop's doms to refer to wrap's vars.
    auto i = lam2sites_.find(lam);
    if (i == lam2sites_.end() || !lam->is_set() || !lam->is_closed() || lam->type()->isa_mut<Pi>())
        return lam2statics_[lam] = Mask();

    const auto& sites = i->second;
    auto n            = lam->num_tdoms();

    // A static *function* arg is what SAT is after, as it exposes the loop's free var to inlining (Santos §7).
    // So seed the split with the rightmost Pi-typed dom and keep only the sites forwarding it; ∀ if there is none.
    auto pool = Vector<bool>(sites.size(), true);
    for (size_t d = n; d-- != 0;) {
        if (!lam->tdom(d)->isa<Pi>()) continue;
        auto seeded = false;
        for (const auto& mask : sites)
            seeded |= mask[d];
        if (seeded)
            for (size_t s = 0, e = sites.size(); s != e; ++s)
                pool[s] = sites[s][d];
        break;
    }

    auto res        = Mask(n, true);
    auto num_static = size_t(0);
    for (size_t s = 0, e = sites.size(); s != e; ++s)
        if (pool[s])
            for (size_t d = 0; d != n; ++d)
                res[d] = res[d] && sites[s][d];
    for (auto r : res)
        num_static += r;
    if (num_static == 0) return lam2statics_[lam] = Mask();

    DLOG("statics for `{}`: {}", lam, fe::Join(res, ", "));
    return lam2statics_[lam] = res;
}

const Def* StaticArgOpt::rewrite_mut_Lam(Lam* old_lam) {
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

const Def* StaticArgOpt::rewrite_imm_App(const App* old_app) {
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
