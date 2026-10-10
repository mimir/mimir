#include "mim/phase.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <fe/bitset.h>

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

const fe::Vector<std::string>& Phase::args() { return driver().args(Annex::demangle(Annex::flags2plugin(annex_))); }

std::unique_ptr<Phase> Phase::recreate() {
    auto ctor = driver().phase(annex());
    auto ptr  = (*ctor)(world());
    ptr->apply(*this);
    return ptr;
}

void Phase::run() {
    auto profiling = driver().flags().profile != Flags::Profile::None;
    if (profiling) driver().profiler().start(name());
    world().verify().log().i("🚀 launch phase `{}`", name());
    {
        auto _ = world().activate();
        start();
    }
    world().verify().log().i("🏁 finish phase `{}`", name());
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
    todo_          = false;
    bootstrapping_ = true; // every full round walks the annexes again - and that walk *is* the bootstrapping half
}

void Analysis::start() {
    curr_sparse_ = !dense_ && !nonlocal_ && !dirty_.empty();
    nonlocal_    = false;
    auto seeds   = fe::Vector<Def*>(dirty_.begin(), dirty_.end());
    dirty_.clear();

    prepare();

    if (curr_sparse_) {
        bootstrapping_ = false; // a sparse round re-drains program muts - it has no annex half
        log().v("sparse round: re-drain {} dirty muts", seeds.size());
        std::ranges::sort(seeds, GIDLt<Def*>()); // MutSet iteration order is nondeterministic

        // Pre-install what earlier rounds substituted, so this round prunes everything they already settled -
        // except for mutables, whose map entry doubles as the per-round "already scheduled" marker and would
        // suppress their drain. Pruning skips rewrite hooks, which is why only a sparse round does it: the
        // full round that certifies the fixed point always re-derives from the program.
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

const Def* Analysis::repr_(const Def* slow, const Def* fast) const {
    while (slow != fast) {
        auto next = follow(fast);
        if (!next) return fast;
        fast = follow(next);
        if (!fast) return next;
        slow = follow(slow);
        assert(slow && "slow lags fast, so fast already traversed slow's successor");
    }

    auto res = slow;
    for (auto def = follow(slow); def != slow; def = follow(def))
        if (def->gid() < res->gid()) res = def;
    return res;
}

const Def* Analysis::rewrite(const Def* def) {
    if (def->isa_mut()) return Rewriter::rewrite(def);
    return repr(Rewriter::rewrite(def));
}

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
        log().d("enter {}", mut);
        for (auto d : mut->deps())
            rewrite(d);
        leave();
    }
}

/*
 * RWPhase
 */

void RWPhase::run() {
    if (!new_world_) retarget(*(new_world_ = old_world().inherit()));
    Phase::run();
}

void RWPhase::start() {
    auto max_iters = driver().flags().max_fp_iters;
    bool todo      = true;
    for (uint32_t i = 0; todo; ++i) {
        if (i >= max_iters) fe::throwf("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        log().v("iteration {}", i);
        todo = analyze();
    }

    // A base Def built while rewriting belongs to the new World; after the swap that is the old one.
    auto _ = new_world().activate();

    // A sealed annex is shared and only rewritten if the program reaches it.
    for (const auto& [flags, e] : old_world().annexes())
        if (e.def->is_sealed()) new_world().annexes().attach(flags, e.sym, e.def);

    // The annex half is a fixed tax proportional to the loaded plugins' annex graph - not to the program.
    auto gid = new_world().curr_gid();
    for (const auto& [flags, e] : old_world().annexes())
        if (!e.def->is_sealed()) rewrite_annex(flags, e.sym, e.def);
    profile_count("rw.defs.annex", new_world().curr_gid() - gid);

    bootstrapping_ = false;

    gid = new_world().curr_gid();
    for (auto mut : old_world().externals().muts())
        rewrite_external(mut);
    finalize(); // inside the span: work deferred by the root walk belongs to the root walk
    profile_count("rw.defs.external", new_world().curr_gid() - gid);

    for (const auto& [flags, e] : old_world().annexes())
        if (auto new_def = e.def->is_sealed() ? lookup(e.def) : nullptr; new_def && new_def != e.def)
            new_world().annexes().reattach(flags, new_def);

    swap(old_world(), new_world());
    retarget(old_world());
    new_world_.reset();
}

const Def* RWPhase::rewrite_mut(Def* old_mut) {
    // Most RWPhase%s leave the shared annex graph alone: keep a base mutable unless rewriting changes it.
    // Code is copied nonetheless, as many RWPhase%s Def::set the Lam%s they get back.
    if (old_mut->is_base()
        && (old_mut->isa<Pi>() || old_mut->isa<Sigma>() || old_mut->isa<Seq>() || old_mut->isa<Variant>())
        && try_keep(old_mut))
        return old_mut;
    return Rewriter::rewrite_mut(old_mut);
}

const Def* RWPhase::rewrite(const Def* old_def) {
    if (bootstrapping_ || !old_def->is_sealed()) return Rewriter::rewrite(old_def);
    // A sealed Def is shared with the annexes, which are rewritten while bootstrapping.
    auto _ = fe::Restore(bootstrapping_, true);
    return Rewriter::rewrite(old_def);
}

bool RWPhase::analyze() {
    if (analysis_) {
        analysis_->reset();
        analysis_->run();
        return analysis_->todo();
    }

    return false;
}

const Def* RWPhase::annex(flags_t flags) {
    auto& e = old_world().annexes().flags2entry().at(flags);
    if (e.def->is_sealed()) return rewrite_root(e.def);
    if (auto new_e = fe::lookup(new_world().annexes().flags2entry(), flags)) return new_e->def;
    return new_world().annexes().attach(flags, e.sym, rewrite_root(e.def));
}

void RWPhase::rewrite_annex(flags_t f, Sym sym, const Def* def) {
    auto new_def = rewrite_root(def);
    if (!new_world().annexes().flags2entry().contains(f)) new_world().annexes().attach(f, sym, new_def);
}

void RWPhase::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_root(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

/*
 * Seal
 */

void Seal::start() {
    auto& base = old_world().base();
    {
        auto _  = base.thaw();
        auto __ = base.activate();
        profile_count("seal.defs", base.absorb(old_world()));
        base.seal(DefVec(old_world().annexes().defs().begin(), old_world().annexes().defs().end()));
    }
    RWPhase::start();
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
    auto all   = fe::Bitset(n, true);
    auto stale = all;
    auto ran   = fe::Bitset(n, false);

    for (uint32_t iter = 0; stale.any(); ++iter) {
        if (iter >= max_iters)
            fe::throwf("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        if (fixed_point()) log().v("🔄 fixed-point iteration {}", iter);

        bool todo = false;
        for (size_t i = 0; i != n; ++i) {
            auto& phase = phases()[i];
            if (!stale.test(i)) {
                log().v("skip `{}`: World unchanged since its last quiet run", phase->name());
                profile_count("phases.skipped");
                continue;
            }

            if (ran.test(i)) { // re-runs need a fresh instance
                auto new_phase = std::unique_ptr<Phase>(static_cast<Phase*>(phase->recreate().release()));
                swap(new_phase, phase);
            }

            phase->run();
            ran.set(i);
            stale.clear(i);

            if (phase->todo()) {
                todo = true;
                // The World changed: everyone - including this phase itself - gets another look.
                stale = all;
            }
        }

        todo &= fixed_point();
        invalidate(todo);
        if (!fixed_point()) break;
    }
}

} // namespace mim
