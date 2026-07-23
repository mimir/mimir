#include "mim/phase.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <absl/container/fixed_array.h>

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

const Vector<std::string>& Phase::args() {
    return driver().args(Annex::demangle(driver(), Annex::flags2plugin(annex_)));
}

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

void Phase::profile_count(std::string_view key, uint64_t n) {
    if (driver().flags().profile != Flags::Profile::None) driver().profiler().count(key, n);
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
    curr_sparse_ = !dense_ && !nonlocal_ && !dirty_.empty();
    nonlocal_    = false;
    auto seeds   = Vector<Def*>(dirty_.begin(), dirty_.end());
    dirty_.clear();

    prepare();

    if (curr_sparse_) {
        VLOG("sparse round: re-draining {} dirty muts", seeds.size());
        std::ranges::sort(seeds, GIDLt<Def*>()); // MutSet iteration order is nondeterministic

        // Replay the lattice into the fresh rewriter map: this re-installs the substitutions that
        // non-revisited producers wrote in earlier rounds - except for mutables, whose map entry
        // doubles as the per-round "already scheduled" marker and would suppress their drain.
        for (auto [concr, abstr] : lattice_)
            if (!concr->isa_mut()) map(concr, abstr);

        for (auto mut : seeds)
            rewrite(mut);
        drain();
    } else {
        for (const auto& [flags, e] : world().annexes())
            rewrite_annex(flags, e.sym, e.def);
        drain();

        bootstrapping_ = false;

        for (auto mut : world().externals().muts())
            rewrite_external(mut);
        drain();

        finalize();
    }

    profile_count(curr_sparse_ ? "rounds.sparse" : "rounds.full");
    profile_count("muts.drained", std::exchange(num_drained_, size_t(0)));

    // A quiet sparse round only certifies the muts it visited:
    // force one full round; the fixed point counts only if that one stays quiet, too.
    if (curr_sparse_ && !todo()) invalidate();
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
        ++num_drained_;

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
    auto max_iters = driver().flags().max_fp_iters;
    bool todo      = true;
    for (uint32_t i = 0; todo; ++i) {
        if (i >= max_iters) error("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        VLOG("iteration: {}", i);
        todo = analyze();
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
    auto max_iters = driver().flags().max_fp_iters;
    auto n         = phases().size();
    // A phase's run is a deterministic function of the World's content.
    // So a phase only needs to run (again) if the World (may have) changed since its last quiet run.
    auto stale = absl::FixedArray<bool>(n, true);
    auto ran   = absl::FixedArray<bool>(n, false);

    auto any_stale = [&stale]() { return std::ranges::any_of(stale, [](bool b) { return b; }); };

    for (uint32_t iter = 0; any_stale(); ++iter) {
        if (iter >= max_iters) error("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        if (fixed_point()) VLOG("🔄 fixed-point iteration: {}", iter);

        bool todo = false;
        for (size_t i = 0; i != n; ++i) {
            auto& phase = phases()[i];
            if (!stale[i]) {
                VLOG("skipping `{}`: World unchanged since its last quiet run", phase->name());
                profile_count("phases.skipped");
                continue;
            }

            if (ran[i]) { // re-runs need a fresh instance
                auto new_phase = std::unique_ptr<Phase>(static_cast<Phase*>(phase->recreate().release()));
                swap(new_phase, phase);
            }

            phase->run();
            ran[i]   = true;
            stale[i] = false;

            if (phase->todo()) {
                todo = true;
                // The World changed: everyone - including this phase itself - gets another look.
                std::ranges::fill(stale, true);
            }
        }

        todo &= fixed_point();
        invalidate(todo);
        if (!fixed_point()) break;
    }
}

} // namespace mim
