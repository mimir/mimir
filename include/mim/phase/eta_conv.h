#pragma once

#include "mim/phase.h"

#include "mim/util/gid.h"

namespace mim {

/// Combined η-normalization: folds η-reduction and η-expansion into a single, idempotent phase.
/// A Lam should appear either **only** in callee position (Known) or not (Unknown).
/// A Lam that occurs in an unknown position more than once (Unknown_N) or in both positions is η-expanded
/// (`g f -> g (λx.f x)`); a genuine η-redex `λx.f x` whose `f` does **not** want to be expanded is η-reduced.
///
/// The analysis is **wrapper-transparent**: a use of a wrapper `λx.f x` is counted as a use of `f` at the same
/// position/multiplicity instead of counting `f` as Known (the wrapper's callee).
/// This makes `f`'s classification identical whether `f` is bare or wrapped, so the canonical η-form is a genuine
/// fixed point - the phase does not fight itself and can share one big `compile.phases tt` fixed-point loop with
/// BetaRed and mem.seo without oscillating.
class EtaConv : public InplaceRWPhase {
public:
    EtaConv(World& world)
        : InplaceRWPhase(world, "EtaConv") {}
    EtaConv(World& world, flags_t annex)
        : InplaceRWPhase(world, annex) {}

private:
    /// Known and Unknown_* are independent bits; both set means "both positions".
    enum Lattice : u8 {
        None      = 0,
        Known     = 1 << 0,
        Unknown_1 = 1 << 1,
        Unknown_N = Unknown_1 | 1 << 2,
    };

    /// Two Unknown_1 uses saturate to Unknown_N.
    static Lattice join(Lattice a, Lattice b) { return Lattice(a | b | (a & b & Unknown_1) << 1); }

    Lattice lattice(const Lam* lam) const {
        if (auto l = fe::lookup(lam2lattice_, lam)) return *l;
        return None;
    }

    /// Does @p def want to be η-expanded - and hence keep any wrapper `λx.def x` instead of reducing it?
    bool expand(const Def* def) const {
        auto lam = def->isa<Lam>();
        return lam && lattice(lam) > Unknown_1;
    }

    /// Does the wrapper @p lam serve this one occurrence alone?
    /// Only then may we keep it - re-creating it would hand out a fresh identity on every run, so this phase would
    /// never reach a fixed point in place.
    bool is_exclusive_wrapper(const Lam* lam) const {
        if (auto n = fe::lookup(wrapper_uses_, lam)) return *n == 1;
        return false;
    }

    void join(const Lam* lam, Lattice l) {
        auto& x = lam2lattice_[lam];
        x       = join(x, l);
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
