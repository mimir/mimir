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
    auto mask = fe::Bitset();
    for (size_t i = 0, n = lam->num_tdoms(); i != n; ++i)
        if (app->targ(i) == lam->tvar(i)) mask.set(i);
    if (mask.any()) lam2sites_[lam].emplace_back(std::move(mask)); // only lam's own body can mention lam's vars
}

Sieve StaticArgOpt::sieve(Lam* lam) {
    if (auto i = lam2sieve_.find(lam); i != lam2sieve_.end()) return i->second;

    auto n = lam->num_tdoms();

    // A *mutable* Pi is a dependent one; splitting it would require loop's doms to refer to wrap's vars.
    auto i = lam2sites_.find(lam);
    if (i == lam2sites_.end() || !lam->is_set() || !lam->is_closed() || lam->type()->isa_mut<Pi>())
        return lam2sieve_.emplace(lam, Sieve(n)).first->second;

    const auto& sites = i->second;

    // A static *function* arg is what SAT is after, as it exposes the loop's free var to inlining (Santos §7).
    // So seed the split with the rightmost Pi-typed dom and keep only the sites forwarding it; ∀ if there is none.
    auto pool = fe::Bitset();
    for (size_t s = 0, e = sites.size(); s != e; ++s)
        pool.set(s);
    for (size_t d = n; d-- != 0;) {
        if (!lam->tdom(d)->isa<Pi>()) continue;
        auto seeded = false;
        for (const auto& mask : sites)
            seeded |= mask[d];
        if (seeded)
            for (size_t s = 0, e = sites.size(); s != e; ++s)
                pool.set(s, sites[s][d]);
        break;
    }

    auto statics = fe::Bitset();
    for (size_t d = 0; d != n; ++d)
        statics.set(d);
    for (size_t s = 0, e = sites.size(); s != e; ++s)
        if (pool[s]) statics &= sites[s];
    if (statics.none()) return lam2sieve_.emplace(lam, Sieve(n)).first->second;

    log().d("statics of {}: {}", lam, statics);
    return lam2sieve_.emplace(lam, Sieve(n, [&statics](size_t d) { return !statics[d]; })).first->second;
}

const Def* StaticArgOpt::rewrite_mut_Lam(Lam* old_lam) {
    if (!is_bootstrapping()) {
        if (auto sieve = this->sieve(old_lam); !sieve.all()) {
            auto& w        = new_world();
            auto n         = old_lam->num_tdoms();
            auto loop_doms = rewrite(sieve.gather(old_lam->tdoms()));

            auto wrap = w.mut_lam(rewrite(old_lam->type())->as<Pi>())->set(old_lam->dbg_key());
            auto loop = w.mut_lam(loop_doms, rewrite(old_lam->codom()))->set(old_lam->dbg_key());
            loop->debug_suffix("_loop");
            log().d("{} → wrap {}, loop {}", old_lam, wrap, loop);
            old2wrap_loop_[old_lam] = {wrap, loop};

            // The body lives in loop; the static vars stay wrap's and are free in loop.
            auto vars = DefVec(n, [&](size_t d) {
                auto j = sieve[d];
                return j == Sieve::Gone ? wrap->tvar(d) : loop->var(sieve.num_new(), j);
            });

            map(old_lam, wrap);
            map(old_lam->var(), vars);
            loop->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
            wrap->app(false, loop, sieve.gather(wrap->tvars()));
            return wrap;
        }
    }

    return RWPhase::rewrite_mut_Lam(old_lam);
}

const Def* StaticArgOpt::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>(); old_lam && !is_bootstrapping()) {
        if (auto sieve = this->sieve(old_lam); !sieve.all()) {
            rewrite(old_lam); // make sure wrap/loop exist
            if (auto i = old2wrap_loop_.find(old_lam); i != old2wrap_loop_.end()) {
                auto loop = i->second.second;
                for (size_t d = 0, n = old_lam->num_tdoms(); d != n; ++d)
                    if (sieve[d] == Sieve::Gone && old_app->targ(d) != old_lam->tvar(d))
                        return RWPhase::rewrite_imm_App(old_app); // not our class: go through wrap
                invalidate();
                return new_world().app(loop, rewrite(sieve.gather(old_app->targs())));
            }
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim
