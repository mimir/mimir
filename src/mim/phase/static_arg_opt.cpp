#include "mim/phase/static_arg_opt.h"

namespace mim {

void StaticArgOpt::apply(bool aggr) {
    aggr_ = aggr;
    name_ += aggr_ ? " tt" : " ff";
}

void StaticArgOpt::apply(const App* app) { apply(Lit::as<bool>(app->arg())); }
void StaticArgOpt::apply(Phase& phase) { apply(static_cast<StaticArgOpt&>(phase).aggr_); }

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
    if (num_static == 0 || !sheds_ret(lam, res)) return lam2statics_[lam] = Mask();

    DLOG("statics for `{}`: {}", lam, fe::Join(res, ", "));
    return lam2statics_[lam] = res;
}

std::pair<StaticArgOpt::Clone, bool> StaticArgOpt::clone(Lam* old_lam, const Mask& d) {
    auto& clones = lam2clones_[old_lam];
    for (const auto& c : clones)
        if (c.d == d) return {c, false};

    auto& w        = new_world();
    auto n         = d.size();
    auto loop_doms = DefVec();
    for (size_t i = 0; i != n; ++i)
        if (!d[i]) loop_doms.emplace_back(rewrite(old_lam->tdom(i)));

    auto wrap = w.mut_lam(rewrite(old_lam->type())->as<Pi>())->set(old_lam->dbg_key());
    auto loop = w.mut_lam(loop_doms, rewrite(old_lam->codom()))->set(old_lam->dbg_key());
    loop->debug_suffix("_loop");
    DLOG("old {} -> (wrap: {}, loop: {})", old_lam, wrap, loop);
    clones.emplace_back(Clone{d, wrap, loop});
    return {clones.back(), true};
}

void StaticArgOpt::specialize(Lam* old_lam, const Clone& c, bool cloned) {
    auto n       = c.d.size();
    auto num_dyn = size_t(0);
    for (auto s : c.d)
        num_dyn += !s;

    auto old_nest = std::unique_ptr<const Nest>();
    if (cloned) {
        push();
        old_nest = std::move(nest_);
        nest_    = std::make_unique<const Nest>(old_lam);
    }

    // The body lives in loop; the static vars stay wrap's and are free in loop.
    DefVec vars(n), args;
    for (size_t i = 0, j = 0; i != n; ++i) {
        if (c.d[i]) {
            vars[i] = c.wrap->tvar(i);
        } else {
            vars[i] = c.loop->var(num_dyn, j++);
            args.emplace_back(c.wrap->tvar(i));
        }
    }

    map(old_lam, c.wrap);
    map(old_lam->var(), vars);
    c.loop->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));

    if (cloned) {
        pop();
        nest_ = std::move(old_nest);
    }
    c.wrap->app(false, c.loop, args);
}

void StaticArgOpt::finalize() {
    while (!todo_clones_.empty()) {
        auto [old_lam, d] = todo_clones_.front();
        todo_clones_.pop_front();
        for (const auto& c : lam2clones_[old_lam])
            if (c.d == d) {
                specialize(old_lam, c, true);
                break;
            }
    }
}

// A clone must not reuse another specialization's rewrites, so bypass the cache for context-dependent Defs.
const Def* StaticArgOpt::rewrite(const Def* old_def) {
    if (nest_ && !old2news_.back().contains(old_def) && nest_->contains(old_def)) {
        auto new_def = old_def->isa_mut() ? RWPhase::rewrite_mut((Def*)old_def) : RWPhase::rewrite_imm(old_def);
        return new_def->set(old_def->dbg_key());
    }
    return RWPhase::rewrite(old_def);
}

const Def* StaticArgOpt::rewrite_mut_Lam(Lam* old_lam) {
    if (!is_bootstrapping()) {
        if (auto s = statics(old_lam); !s.empty()) {
            auto [c, _] = clone(old_lam, s);
            specialize(old_lam, c, false);
            return c.wrap;
        }
    }

    return RWPhase::rewrite_mut_Lam(old_lam);
}

const Def* StaticArgOpt::rewrite_imm_App(const App* old_app) {
    if (auto old_lam = old_app->callee()->isa_mut<Lam>(); old_lam && !is_bootstrapping()) {
        if (auto s = statics(old_lam); !s.empty()) {
            rewrite(old_lam); // make sure the canonical wrap/loop exist
            auto n    = s.size();
            auto cur  = lookup(old_lam); // the specialization we are inside, if any
            auto mask = Mask(n, false);
            for (size_t i = 0; i != n; ++i)
                mask[i] = old_app->targ(i) == old_lam->tvar(i);

            auto enc     = Clone();
            auto has_enc = false;
            if (auto i = lam2clones_.find(old_lam); i != lam2clones_.end())
                for (const auto& c : i->second)
                    if (c.wrap == cur) {
                        enc = c, has_enc = true;
                        break;
                    }

            auto jump = [&](const Clone& c) {
                auto args = DefVec();
                for (size_t i = 0; i != n; ++i)
                    if (!c.d[i]) args.emplace_back(rewrite(old_app->targ(i)));
                return new_world().app(c.loop, args);
            };

            auto any = false;
            for (auto b : mask)
                any |= b;

            // Aggressively give this site the specialization for *its* statics, even if it fits the enclosing one.
            if (aggr_ && any && sheds_ret(old_lam, mask)) {
                if (has_enc && enc.d == mask) return jump(enc);
                auto [c, fresh] = clone(old_lam, mask);
                if (fresh) todo_clones_.emplace_back(old_lam, mask);
                auto args = DefVec(n, [&](size_t i) { return rewrite(old_app->targ(i)); });
                return new_world().app(c.wrap, args);
            }

            if (has_enc) {
                auto ok = true;
                for (size_t i = 0; i != n; ++i)
                    ok &= !enc.d[i] || mask[i];
                if (ok) return jump(enc);
            }
        }
    }

    return RWPhase::rewrite_imm_App(old_app);
}

} // namespace mim
