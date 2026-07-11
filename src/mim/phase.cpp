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
    prepare();

    full_round_ = !sparse_ || round_ == 0 || need_full_ || dirty_.empty();
    certifying_ = need_full_;
    need_full_  = false;
    std::swap(dirty_prev_, dirty_);
    dirty_.clear();
    tracking_ = true;

    if (full_round_) {
        for (const auto& [flags, e] : world().annexes())
            rewrite_annex(flags, e.sym, e.def);
        drain();

        bootstrapping_ = false;

        for (auto mut : world().externals().muts())
            rewrite_external(mut);
        drain();

        tracking_ = false;
        finalize(); // only full rounds finalize: post-passes must see the complete abstract World
    } else {
        VLOG("sparse round {}: re-draining {} tainted muts", round_, dirty_prev_.size());
        for (auto [concr, abstr] : set_map_)
            map(concr, abstr);
        for (auto mut : dirty_prev_)
            rewrite(mut);
        drain();
        tracking_ = false;
    }

    ++round_;
    profile_count(full_round_ ? "rounds.full" : "rounds.sparse");
    profile_count("muts.drained", std::exchange(num_drained_, size_t(0)));

    // Sparse rounds quiesced - but they only certify the muts they visited.
    // Force one full round; the fixed point counts only if that one stays quiet, too.
    if (sparse_ && !full_round_ && !todo()) {
        need_full_ = true;
        invalidate();
    }
}

void Analysis::rewrite_annex(flags_t, Sym, const Def* def) { rewrite(def); }
void Analysis::rewrite_external(Def* mut) { rewrite(mut); }

Def* Analysis::rewrite_mut(Def* mut) {
    if (lookup(mut)) return mut; // already scheduled this round
    map(mut, mut);
    // In a sparse round only tainted muts are drained; all others just map to themselves.
    if (full_round_ || mut->is_dirty()) worklist_.emplace_back(mut);
    return mut;
}

void Analysis::drain() {
    while (!worklist_.empty()) {
        auto mut = worklist_.front();
        worklist_.pop_front();
        mut->dirty(false); // this is the (re)visit the dirty bit asked for
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

    // Translate the dirt - recorded in terms of old muts - into the new world so it survives the swap.
    auto old_dirty = std::exchange(dirty_, {});
    for (auto old_mut : old_dirty)
        if (auto new_def = lookup(old_mut))
            if (auto new_mut = new_def->isa_mut()) taint(new_mut);

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
