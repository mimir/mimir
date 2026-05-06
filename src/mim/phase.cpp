#include "mim/phase.h"

#include <memory>

#include "mim/driver.h"

namespace mim {

/*
 * Phase
 */

void Phase::run() {
    world().verify().ILOG("🚀 Phase launch: `{}`", name());
    start();
    world().verify().ILOG("🏁 Phase finish: `{}`", name());
}

/*
 * Analyzer
 */

void Analysis::reset() {
    old2news_.clear();
    push();
    todo_ = false;
}

void Analysis::start() {
    for (const auto& [f, def] : world().flags2annex())
        rewrite_annex(f, def);

    bootstrapping_ = false;

    for (auto mut : world().externals().muts())
        rewrite_external(mut);
}

void Analysis::rewrite_annex(flags_t, const Def* def) { rewrite(def); }
void Analysis::rewrite_external(Def* mut) { rewrite(mut); }

Def* Analysis::rewrite_mut(Def* mut) {
    map(mut, mut);

    if (auto var = mut->has_var()) {
        map(var, var);

        if (mut->isa<Lam>())
            for (auto var : mut->tvars()) {
                map(var, var);
                lattice_[var] = var;
            }
    }

    for (auto d : mut->deps())
        rewrite(d);

    return mut;
}

/*
 * RWPhase
 */

void RWPhase::start() {
    int i = 0;
    for (bool todo = true; todo;) {
        VLOG("iteration: {}", i++);
        todo = false;
        todo |= analyze();
    }

    for (const auto& [f, def] : old_world().flags2annex())
        rewrite_annex(f, def);

    bootstrapping_ = false;

    for (auto mut : old_world().externals().muts())
        rewrite_external(mut);

    swap(old_world(), new_world());
}

bool RWPhase::analyze() {
    if (analysis_) {
        analysis_->reset();
        analysis_->run();
        return analysis_->todo();
    }

    return false;
}

void RWPhase::rewrite_annex(flags_t f, const Def* def) { new_world().register_annex(f, rewrite(def)); }

void RWPhase::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

/*
 * ReplMan
 */

void ReplMan::apply(Repls&& repls) {
    for (auto&& repl : repls)
        if (auto&& man = repl->isa<ReplMan>())
            apply(std::move(man->repls_));
        else
            add(std::move(repl));
}

void ReplMan::apply(const App* app) {
    auto repls = Repls();
    for (auto arg : app->args())
        if (auto stage = Stage::create(driver().stages(), arg))
            repls.emplace_back(std::unique_ptr<Repl>(static_cast<Repl*>(stage.release())));

    apply(std::move(repls));
}

/*
 * ReplManPhase
 */

void ReplManPhase::apply(const App* app) {
    man_       = std::make_unique<ReplMan>(old_world(), annex());
    auto repls = Repls();
    for (auto arg : app->args())
        if (auto stage = Phase::create(driver().stages(), arg))
            repls.emplace_back(std::unique_ptr<Repl>(static_cast<Repl*>(stage.release())));
    man_->apply(std::move(repls));
}

void ReplManPhase::apply(Stage& stage) {
    auto& rmp = static_cast<ReplManPhase&>(stage);
    swap(man_, rmp.man_);
}

void ReplManPhase::start() {
    old_world().verify().ILOG("🔥 run");
    for (auto&& repl : man().repls())
        ILOG(" 🔹 `{}`", repl->name());
    old_world().debug_dump();
    RWPhase::start();
}

const Def* ReplManPhase::rewrite(const Def* def) {
    for (bool todo = true; todo;) {
        todo = false;
        for (auto&& repl : man().repls())
            if (auto subst = repl->replace(def)) todo = true, def = subst;
    }

    return Rewriter::rewrite(def);
}

/*
 * PhaseMan
 */

void PhaseMan::apply(bool fp, Phases&& phases) {
    fixed_point_ = fp;
    phases_      = std::move(phases);
    name_ += fixed_point_ ? " tt" : " ff";
}

void PhaseMan::apply(const App* app) {
    auto [fp, args] = app->uncurry_args<2>();

    auto phases = Phases();
    for (auto arg : args->projs())
        if (auto stage = create(driver().stages(), arg)) {
            // clang-format off
            if (auto pm = stage->isa<PassManPhase>(); pm && pm->  man().empty()) continue;
            if (auto rp = stage->isa<ReplMan     >(); rp && rp->repls().empty()) continue;
            // clang-format on
            phases.emplace_back(std::unique_ptr<Phase>(static_cast<Phase*>(stage.release())));
        }

    apply(Lit::as<bool>(fp), std::move(phases));
}

void PhaseMan::apply(Stage& stage) {
    auto& man = static_cast<PhaseMan&>(stage);
    Phases new_phases;
    for (auto& old_phase : man.phases())
        new_phases.emplace_back(std::unique_ptr<Phase>(static_cast<Phase*>(old_phase->recreate().release())));
    apply(man.fixed_point(), std::move(new_phases));
}

void PhaseMan::start() {
    int iter = 0;
    for (bool todo = true; todo; ++iter) {
        todo = false;

        if (fixed_point()) VLOG("🔄 fixed-point iteration: {}", iter);

        for (auto& phase : phases()) {
            phase->run();
            todo |= phase->todo();
        }

        todo &= fixed_point();

        if (todo) {
            for (auto& old_phase : phases()) {
                auto new_phase = std::unique_ptr<Phase>(static_cast<Phase*>(old_phase->recreate().release()));
                swap(new_phase, old_phase);
            }
        }

        invalidate(todo);
    }
}

/*
 * PassManPhase
 */

void PassManPhase::apply(const App* app) {
    man_        = std::make_unique<PassMan>(world(), annex());
    auto passes = Passes();
    for (auto arg : app->args())
        if (auto stage = Phase::create(driver().stages(), arg))
            passes.emplace_back(std::unique_ptr<Pass>(static_cast<Pass*>(stage.release())));

    man_->apply(std::move(passes));
}

void PassManPhase::apply(Stage& stage) {
    auto& pmp = static_cast<PassManPhase&>(stage);
    swap(man_, pmp.man_);
}

} // namespace mim
