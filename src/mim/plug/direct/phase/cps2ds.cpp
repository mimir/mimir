#include "mim/plug/direct/phase/cps2ds.h"

#include <ranges>

#include "mim/def.h"
#include "mim/rewrite.h"
#include "mim/schedule.h"
#include "mim/world.h"

#include "mim/plug/direct/direct.h"

#define DEBUG_CPS2DS 0

namespace mim::plug::direct {

void CPS2DSPhase::start() {
#if DEBUG_CPS2DS
    world().debug_dump();
#endif

    scheduler_.clear();
    nests_.clear();
    lam2lam_.clear();
    rewritten_.clear();

    world().for_each(true, [this](Def* mut) {
        if (auto lam = mut->isa_mut<Lam>(); lam && !lam->codom()->isa<Type>())
            nests_.try_emplace(lam, std::make_unique<Nest>(lam));
    });

    for (auto external : world().externals().muts())
        if (auto lam = external->isa_mut<Lam>()) {
            current_external_ = lam;
            rewrite_lam(lam);
        }

#if DEBUG_CPS2DS
    world().debug_dump();
#endif
}

const Def* CPS2DSPhase::rewrite_lam(Lam* lam) {
    if (auto i = rewritten_.find(lam); i != rewritten_.end()) return i->second;
    if (lam2lam_.contains(lam)) return lam;
    if (lam->isa_imm() || !lam->is_set() || lam->codom()->isa<Type>()) {
        world().DLOG("skipped {}", lam);
        return lam;
    }

    lam2lam_[lam] = lam;

    world().DLOG("Rewriting lam: {}", lam->unique_name());

    auto filter = rewrite(lam->filter());

    if (auto body = lam->body()->isa<App>(); !body) {
        world().DLOG("  non-app body {}, skipped", lam->body());
        auto new_body = rewrite(lam->body());
        lam->unset()->set(filter, new_body);
        return rewritten_[lam] = lam;
    }

    auto body    = lam->body()->as<App>();
    auto new_arg = rewrite(body->arg());

    auto new_callee = rewrite(body->callee());
    auto new_lam    = result_lam(lam);
#if DEBUG_CPS2DS
    world().DLOG("Result of rewrite {} set for {}", lam->unique_name(), new_lam->unique_name());
#endif

    if (world().log().level() >= mim::Log::Level::Debug) body->dump(1);

    new_lam->unset()->app(filter, new_callee, new_arg);

    return rewritten_[lam] = lam;
}

const Def* CPS2DSPhase::rewrite(const Def* def) {
    if (auto i = rewritten_.find(def); i != rewritten_.end()) return i->second;

    if (auto lam = def->isa_mut<Lam>()) return rewrite_lam(lam);

    if (auto app = def->isa<App>()) {
        if (auto cps2ds = Axm::isa<direct::cps2ds_dep>(app->callee())) {
            auto cps_lam = rewrite(cps2ds->arg())->as<Lam>();

            auto call_arg = rewrite(app->arg());

            if (world().log().level() >= mim::Log::Level::Debug) {
                cps2ds->dump(2);
                cps2ds->arg()->dump(2);
            }

            auto early = scheduler(app).early(app);
            auto late  = scheduler(app).late(current_external_, app);
            auto node  = Nest::lca(early, late);
#if DEBUG_CPS2DS
            world().DLOG("scheduling {} between {} (level {}) and {} (level {}) at {}", app,
                         early->mut() ? early->mut()->unique_name() : "root", early->level(),
                         late->mut() ? late->mut()->unique_name() : "root", late->level(),
                         node->mut() ? node->mut()->unique_name() : "root");
#endif
            auto lam = result_lam(node->mut()->as_mut<Lam>());

#if DEBUG_CPS2DS
            world().DLOG("current lam: {} : {}", lam->unique_name(), lam->type());
#endif

            auto cn_dom = cps_lam->ret_dom();
            auto cont   = make_continuation(cn_dom, app, cps_lam->sym());
#if DEBUG_CPS2DS
            world().DLOG("continuation created: {} : {}", cont, cont->type());
            if (world().log().level() >= mim::Log::Level::Debug) cont->dump(2);
#endif
            {
                auto filter = rewritten_[lam->filter()] = rewrite(lam->filter());
                auto body                               = world().app(cps_lam, world().tuple({call_arg, cont}));
                rewritten_[lam]                         = lam->unset()->set(filter, body);
                lam2lam_[lam]                           = cont;
            }

#if DEBUG_CPS2DS
            world().DLOG("point the lam to the cont: {} = {}", lam->unique_name(), lam->body());
            if (world().log().level() >= mim::Log::Level::Debug) {
                lam->dump(2);
                cont->dump(2);
            }
#endif

            return cont->var(); // rewritten_[def] = cont->var(); done in make_continuation
        }
    }

    DefVec new_ops{def->ops(), [this](const Def* d) { return rewrite(d); }};
    auto new_def    = def->rebuild(def->type(), new_ops);
    rewritten_[def] = new_def;
    return new_def;
}

Lam* CPS2DSPhase::make_continuation(const Def* cn_type, const Def* arg, Sym prefix) {
#if DEBUG_CPS2DS
    world().DLOG("make_continuation {} : {} ({})", prefix, cn_type, arg);
    if (world().log().level() >= mim::Log::Level::Debug) arg->dump(2);
#endif
    auto name = world().append_suffix(prefix, "_cps2ds_cont");
    auto cont = world().mut_con(cn_type)->set(name)->set_filter(false);

    rewritten_[arg] = cont->var();

    return cont;
}

Lam* CPS2DSPhase::result_lam(Lam* lam) {
    if (auto i = lam2lam_.find(lam); i != lam2lam_.end())
        if (i->second != lam) return result_lam(i->second);
    return lam;
}

Scheduler& CPS2DSPhase::scheduler(const Def* def) {
    auto get_or_make = [&](const Def* lam, const Nest& nest) -> Scheduler& {
        if (auto sched = scheduler_.find(lam); sched != scheduler_.end()) {
#if DEBUG_CPS2DS
            world().DLOG("found existing scheduler for {}", lam);
#endif
            return sched->second;
        } else {
#if DEBUG_CPS2DS
            world().DLOG("creating new scheduler for {}", lam);
#endif
            auto [it, inserted] = scheduler_.insert({lam, Scheduler(nest)});
            return it->second;
        }
    };
    for (const auto& [lam, nest] : nests_) {
#if DEBUG_CPS2DS
        world().DLOG("looking for scheduler in {} for {}", lam, def);
#endif
        if (nest->contains(def)) return get_or_make(lam, *nest);
    }
#if DEBUG_CPS2DS
    world().DLOG("no scheduler found for {}, using current external {}", def, current_external_);
#endif
    return get_or_make(current_external_, curr_external_nest());
}

const Nest& CPS2DSPhase::curr_external_nest() const {
    auto i = nests_.find(current_external_);
    assert(i != nests_.end());
    return *i->second;
}

} // namespace mim::plug::direct
