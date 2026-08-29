#pragma once

#include <fe/assert.h>

#include "mim/phase.h"

#include "mim/util/gid.h"

namespace mim {

/// Combined η-normalization: folds η-reduction and η-expansion into a single, idempotent phase.
/// A Lam should appear either **only** in callee position (Known) or not (Unknown).
/// A Lam that occurs in an unknown position more than once (Unknown_N) or in both positions (Both) is η-expanded
/// (`g f -> g (λx.f x)`); a genuine η-redex `λx.f x` whose `f` does **not** want to be expanded is η-reduced.
///
/// The analysis is **wrapper-transparent**: a use of a wrapper `λx.f x` is counted as a use of `f` at the same
/// position/multiplicity instead of counting `f` as Known (the wrapper's callee).
/// This makes `f`'s classification identical whether `f` is bare or wrapped, so the canonical η-form is a genuine
/// fixed point - the phase does not fight itself and can share one big `%%compile.phases tt` fixed-point loop with
/// BetaRed and %%mem.seo without oscillating.
class EtaConv : public InplaceRWPhase {
public:
    EtaConv(World& world)
        : InplaceRWPhase(world, "EtaConv") {}
    EtaConv(World& world, flags_t annex)
        : InplaceRWPhase(world, annex) {}

private:
    enum Lattice : u8 {
        None      = 0,
        Known     = 1,
        Unknown_1 = 2,
        Unknown_N = 3,
        Both      = 4,
    };

    static Lattice join(Lattice l1, Lattice l2) {
        if (l1 == Unknown_1 && l2 == Unknown_1) return Unknown_N;
        if (l1 == l2) return l1;
        if (l1 == None) return l2;
        if (l2 == None) return l1;
        if (l1 == Both || l2 == Both) return Both;
        if (l1 == Known && (l2 == Unknown_1 || l2 == Unknown_N)) return Both;
        if (l2 == Known && (l1 == Unknown_1 || l1 == Unknown_N)) return Both;
        if (l1 == Unknown_1 && l2 == Unknown_N) return Unknown_N;
        if (l2 == Unknown_1 && l1 == Unknown_N) return Unknown_N;
        fe::unreachable();
    }

    Lattice lattice(const Lam* lam) {
        if (auto i = lam2lattice_.find(lam); i != lam2lattice_.end()) return i->second;
        return None;
    }

    static bool eta_expand(Lattice l) { return l != Known && l != Unknown_1 && l != None; }
    bool eta_expand(const Lam* lam) { return eta_expand(lattice(lam)); }

    /// Should a wrapper `λx.f x` be kept (because `f` wants to be expanded) instead of reduced?
    bool keep_wrapper(const Def* f) {
        auto lam = f->isa<Lam>();
        return lam && eta_expand(lattice(lam));
    }

    /// Is @p lam a wrapper that is already in the shape a fresh Lam::eta_expand would produce here?
    /// That means: it belongs to this one occurrence alone and carries the canonical `tt` filter.
    /// Only then may we keep it - re-creating it would hand out a fresh identity on every run, so this phase would
    /// never reach a fixed point in place.
    bool is_canonical_wrapper(const Lam* lam) const {
        auto i = wrapper_uses_.find(lam);
        return i != wrapper_uses_.end() && i->second == 1 && lam->filter() == lam->world().lit_tt();
    }

    void join(const Lam* lam, Lattice l) {
        if (auto [i, ins] = lam2lattice_.emplace(lam, l); !ins) i->second = join(i->second, l);
    }

    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, Lattice);

    /// An annex or external must keep its shape: neither η-reduce nor η-expand a root.
    const Def* rewrite_root(const Def* def) final { return rewrite_no_eta(def); }
    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Var(const Var*) final;
    /// η-reduce wrappers but never η-expand - used for callee (Known) positions, where expansion must not happen
    /// but a wrapper `λx.f x` should still collapse to `f` (just as the standalone EtaRed did everywhere).
    const Def* rewrite_no_exp(const Def* old_def);
    const Def* rewrite_no_eta(const Def* old_def) { return Rewriter::rewrite(old_def); }

    DefSet analyzed_;
    GIDMap<const Lam*, Lattice> lam2lattice_;
    GIDMap<const Lam*, u32> wrapper_uses_; ///< How many occurrences does a wrapper `λx.f x` serve?
};

} // namespace mim
