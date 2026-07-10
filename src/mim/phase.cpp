#include "mim/phase.h"

#include <memory>

#include "mim/driver.h"
#include "mim/flags.h"

namespace mim {

/*
 * Phase
 */

Phase::Phase(World& world, flags_t annex)
    : world_(world)
    , annex_(annex)
    , name_(world.annex(annex)->sym()) {}

std::unique_ptr<Phase> Phase::recreate() {
    auto ctor = driver().phase(annex());
    auto ptr  = (*ctor)(world());
    ptr->apply(*this);
    return ptr;
}

void Phase::run() {
    auto profiling = driver().flags().profile != Flags::Profile::None;
    if (profiling) driver().profiler().start(name());
    world().verify().ILOG("🚀 Phase launch: `{}`", name());
    start();
    world().verify().ILOG("🏁 Phase finish: `{}`", name());
    if (profiling) driver().profiler().stop();
}

/*
 * Analyzer
 */

void Analysis::reset() {
    old2news_.clear();
    worklist_.clear();
    push();
    todo_ = false;
}

void Analysis::start() {
    prepare();

    for (const auto& [flags, e] : world().annexes())
        rewrite_annex(flags, e.sym, e.def);
    drain();

    bootstrapping_ = false;

    for (auto mut : world().externals().muts())
        rewrite_external(mut);
    drain();

    finalize();
}

const Def* Analysis::pin_top(const Def* def) {
    if (auto [i, ins] = lattice_.emplace(def, def); ins || i->second != def) {
        i->second = def;
        invalidate();
    }
    return def;
}

void Analysis::rewrite_annex(flags_t, Sym, const Def* def) { rewrite(def); }
void Analysis::rewrite_external(Def* mut) { rewrite(mut); }

Def* Analysis::rewrite_mut(Def* mut) {
    if (lookup(mut)) return mut; // already scheduled this round
    map(mut, mut);
    worklist_.emplace_back(mut);
    return mut;
}

void Analysis::drain() {
    while (!worklist_.empty()) {
        auto mut = worklist_.front();
        worklist_.pop_front();

        auto _ = enter(mut);
        DLOG("enter: {}", mut);
        for (auto d : mut->deps())
            rewrite(d);
    }
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

    for (const auto& [flags, e] : old_world().annexes())
        rewrite_annex(flags, e.sym, e.def);

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

void RWPhase::rewrite_annex(flags_t f, Sym sym, const Def* def) { new_world().annexes().attach(f, sym, rewrite(def)); }

void RWPhase::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
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
        if (auto phase = create(driver().phases(), arg)) phases.emplace_back(std::move(phase));

    apply(Lit::as<bool>(fp), std::move(phases));
}

void PhaseMan::apply(Phase& phase) {
    auto& man = static_cast<PhaseMan&>(phase);
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

} // namespace mim
