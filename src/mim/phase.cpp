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

std::unique_ptr<Phase> Phase::create(const Flags2Phases& phases, const Def* def) {
    auto& world = def->world();
    auto p_def  = App::uncurry_callee(def);
    world.log().d("apply phase {}", p_def);

    if (auto axm = p_def->isa<Axm>())
        if (auto i = phases.find(axm->flags()); i != phases.end()) {
            auto phase = i->second(world);
            if (phase) {
                phase->apply(def->isa<App>());
                if (phase->redirects()) return phase->take_resolved();
            }
            return phase;
        } else
            fe::throwf("no phase registered for stage `{}`; is its plugin loaded?", axm->sym());
    else
        fe::throwf("unsupported callee for a phase: `{}`", p_def);
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
    world().verify().log().i("🚀 launch phase `{}`", name());
    start();
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
    todo_ = false;
}

void Analysis::start() {
    curr_sparse_ = !dense_ && !nonlocal_ && !dirty_.empty();
    nonlocal_    = false;
    auto seeds   = fe::Vector<Def*>(dirty_.begin(), dirty_.end());
    dirty_.clear();

    prepare();

    if (curr_sparse_) {
        log().v("sparse round: re-drain {} dirty muts", seeds.size());
        std::ranges::sort(seeds, GIDLt<Def*>()); // MutSet iteration order is nondeterministic

        // Pre-install what earlier rounds substituted, so this round prunes everything they already settled -
        // except for mutables, whose map entry doubles as the per-round "already scheduled" marker and would
        // suppress their drain. Pruning skips rewrite hooks, which is why only a sparse round does it: the
        // full round that certifies the fixed point always re-derives from the program.
        old2new().reserve(lattice_.size());
        for (auto [concr, abstr] : lattice_)
            if (!concr->isa_mut()) map(concr, abstr);

        for (auto mut : seeds)
            rewrite(mut);
        drain();
    } else {
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

void Analysis::rewrite_external(Def* mut) { rewrite(mut); }

const Def* Analysis::lookup(const Def* def) {
    if (auto res = Rewriter::lookup(def)) return res;
    return world().annexes().opaque(def) ? def : nullptr;
}

bool Analysis::lattice_force(const Def* concr, const Def* abstr) {
    map(concr, abstr);

    if (auto [i, ins] = lattice_.emplace(concr, abstr); !ins) {
        if (i->second == abstr) return false;
        i->second = abstr;
    }
    return touch(), true;
}

bool Analysis::lattice(const Def* concr, const Def* abstr) {
    map(concr, abstr);

    if (auto [i, ins] = lattice_.emplace(concr, abstr); !ins) {
        assert((i->second != concr || abstr == concr) && "monotonicity violation: must not descend from ⊤");
        if (i->second == abstr) return false;
        i->second = abstr;
    } else if (concr == abstr) {
        return false;
    }
    return touch(), true;
}

bool Analysis::is_cyclic(const Def* def) const {
    auto slow = follow(def), fast = follow(slow);
    for (; slow != fast; slow = follow(slow), fast = follow(follow(fast))) {}
    return follow(fast) != fast;
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

void RWPhase::start() {
    auto max_iters = driver().flags().max_fp_iters;
    bool todo      = true;
    for (uint32_t i = 0; todo; ++i) {
        if (i >= max_iters) fe::throwf("phase `{}` did not reach a fixed point after {} iterations", name(), max_iters);
        log().v("iteration {}", i);
        todo = analyze();
    }

    auto gid = new_world().curr_gid();
    for (auto mut : old_world().externals().muts())
        rewrite_external(mut);
    finalize();
    profile_count("rw.defs", new_world().curr_gid() - gid);

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

const Def* RWPhase::lookup(const Def* old_def) {
    if (auto new_def = Rewriter::lookup(old_def)) return new_def;
    if (auto flags = old_world().annexes().opaque(old_def)) return map_root(old_def, new_world().annex(*flags));
    return nullptr;
}

void RWPhase::rewrite_external(Def* old_mut) {
    // A phase may have externalized the new mutable itself already, or replaced it by an immutable.
    if (auto new_mut = rewrite(old_mut)->isa_mut(); new_mut && !new_mut->is_external()) new_mut->externalize();
}

const Def* ReprPhase::rewrite_imm_App(const App* app) {
    // The arg goes first for the same reason as in Rewriter::rewrite_imm_App.
    auto arg    = rewrite(app->arg());
    auto callee = rewrite(app->callee());
    if (auto pi = callee->isa_type<Pi>()) arg = coerce(arg, pi->dom());
    auto res = new_world().app(callee, arg);
    // A partially applied Axm must stay one, as its normalizer only fires once it is saturated.
    if (app->axm() && app->curry() != 0) return res;
    return coerce(res, rewrite(app->type()));
}

/// The type of component @p i of the @p n-ary @p type, whose preceding components are @p parts.
static const Def* component(const Def* type, Defs parts, size_t n, size_t i) {
    if (auto sigma = type->isa_mut<Sigma>())
        if (auto var = sigma->has_var(); var && sigma->op(i)->has_free_var(var)) {
            auto& w   = type->world();
            auto args = DefVec(n, [&](size_t k) { return k < i ? parts[k] : var->proj(n, k); });
            return w.reduce(var, w.tuple(args), i);
        }
    return type->proj(n, i);
}

const Def* ReprPhase::rewrite_imm_Tuple(const Tuple* tuple) {
    auto type = rewrite(tuple->type());
    auto ops  = rewrite(tuple->ops());
    auto n    = ops.size();
    if (Lit::isa(type->arity()) != n) return coerce(new_world().tuple(ops), type);
    for (size_t i = 0; i != n; ++i)
        ops[i] = coerce(ops[i], component(type, ops, n, i));
    return new_world().tuple(type, ops);
}

const Def* ReprPhase::rewrite_stub(Def* old_mut, Def* new_mut) {
    auto old_lam = old_mut->isa<Lam>();
    auto new_lam = new_mut->isa<Lam>();
    if (!old_lam || !new_lam || !old_lam->is_set()) return Rewriter::rewrite_stub(old_mut, new_mut);

    map(old_mut, new_mut);
    auto _     = enter(old_mut);
    auto codom = new_lam->type()->reduce(new_lam->var());
    new_lam->set(rewrite(old_lam->filter()), coerce(rewrite(old_lam->body()), codom));
    return seal(old_mut, new_mut);
}

/// Does the *new* @p type carry no information?
static bool is_free(const Def* type) {
    if (type->world().inhabitant(type)) return true;
    if (auto sigma = type->isa_imm<Sigma>()) return std::ranges::all_of(sigma->ops(), is_free);
    if (auto arr = type->isa_imm<Arr>()) return is_free(arr->body());
    return false;
}

/// The sole inhabitant of the *new*, information-free @p type.
static const Def* sole(const Def* type) {
    auto& w = type->world();
    if (auto s = w.inhabitant(type)) return s;
    if (auto sigma = type->isa_imm<Sigma>()) return w.tuple(sigma, DefVec(sigma->ops(), sole));
    return w.pack(type->as<Arr>()->shape(), sole(type->as<Arr>()->body()));
}

const Def* ReprPhase::coerce(const Def* val, const Def* type) {
    if (auto res = Checker::assignable(type, val)) return res;
    auto todo = DefVec{val};
    auto res  = reshape(todo, type);
    while (!todo.empty() && is_free(todo.back()->type()))
        todo.pop_back();
    if (!todo.empty()) fe::throwf("cannot coerce `{}` to `{}`", val, type);
    return res;
}

const Def* ReprPhase::reshape(DefVec& todo, const Def* type) {
    auto& w    = new_world();
    auto front = todo.empty() ? nullptr : todo.back();
    if (front) {
        if (auto res = Checker::assignable(type, front)) return todo.pop_back(), res;
        if (auto res = eta_coerce(front, type)) return todo.pop_back(), res;
    }

    if (is_free(type)) return sole(type);
    if (front && is_free(front->type())) return todo.pop_back(), reshape(todo, type);

    if ((type->isa<Sigma>() || type->isa_imm<Arr>()) && type->num_tprojs() != 1) {
        auto n     = type->num_tprojs();
        auto parts = DefVec();
        for (size_t i = 0; i != n; ++i)
            parts.emplace_back(reshape(todo, component(type, parts, n, i)));
        return w.tuple(type, parts);
    }

    if (front && front->type()->num_tprojs() != 1) {
        todo.pop_back();
        auto pieces = front->tprojs();
        todo.insert(todo.end(), pieces.rbegin(), pieces.rend());
        return reshape(todo, type);
    }

    fe::throwf("cannot coerce to `{}`", type);
}

const Def* ReprPhase::eta_coerce(const Def* val, const Def* type) {
    auto dst = type->isa<Pi>();
    auto src = val->type()->isa<Pi>();
    if (!dst || !src) return nullptr;

    auto eta   = new_world().mut_lam(dst)->set(val->dbg_key());
    auto arg   = coerce(eta->var(), src->dom());
    auto codom = dst->reduce(eta->var());
    log().d("coerce {}: {} → {}", val, src, dst);
    // An Axm reduces a Lam it is handed, which must not merely find a call to the reshaped one.
    if (auto lam = Lam::isa_rewritable(val)) {
        auto ops = lam->reduce(arg);
        return eta->set(ops[0], coerce(ops[1], codom));
    }
    return eta->set(false, coerce(new_world().app(val, arg), codom));
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
        new_phases.emplace_back(old_phase->recreate());
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

            if (ran.test(i)) phase = phase->recreate(); // re-runs need a fresh instance

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
