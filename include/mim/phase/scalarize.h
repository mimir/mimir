#pragma once

#include <fe/bitset.h>

#include "mim/phase.h"

namespace mim {

/// Scalarization (= argument simplification):
/// ```
/// f := λ (x_1: [T_1, T_2], .., x_n: T_n).E
/// ```
/// becomes
/// ```
/// f' := λ (y_1: T_1, y_2: T_2, .. y_n: T_n).E[x_1 \ (y_1, y_2); ..; x_n \ y_n]
/// ```
/// It is type-directed: an immutable `Cn` is flattened along with all its Lam%s and App%s, even escaping ones.
/// A flattened value is coerce()d where it meets an unflattened shape, e.g. an Axm's signature.
/// A Pi is pinned where a coercion would be observable:
/// * a bare function argument of an Axm (`autodiff.ad f`),
/// * a `Cn` an interface Lam's signature reaches through a type coerce() cannot take apart (`mem.Ptr0 K`),
/// * a value inside a dependently-typed aggregate (a typed closure), or
/// * a parameter indexed non-constantly or consumed whole by an Axm - only this parameter is kept.
///
/// Each run flattens one level; the fixed-point pipeline repeats it.
/// Flags::scalarize_threshold bounds which Arr%s are expanded.
class Scalarize : public CoercePhase {
private:
    /// Optimistic: every immutable `Cn` is flattenable until pinned.
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "Scalarize::Analysis") {}

        /// The parameters of @p type to flatten one level; empty means leave it untouched.
        fe::Bitset plan(const Def* type) const;

    private:
        const Def* rewrite(const Def* old) final;

        void inspect(const Def* def);
        /// Keeps parameter @p dom of @p pi whole.
        void keep(const Pi* pi, size_t dom);
        /// Keeps @p def whole if it is a Lam's Var or one of its parameters.
        void keep_param(const Def* def);
        bool kept(const Pi* pi, size_t dom) const; ///< Is parameter @p dom of @p pi kept whole?
        /// pin%s the `Cn`%s below the parts of @p type that coerce() cannot take apart.
        void pin_opaque(DefSet& structural, const Def* type);

        /// Not in lattice(), as its map() of the Pi would break type arguments like `mem.slot (H, 0)`.
        DefMap<fe::Bitset> keeps_;
    };

public:
    Scalarize(World& world, flags_t annex)
        : CoercePhase(world, annex, &analysis_)
        , analysis_(world) {}

private:
    /// Rebuilds what an interface Lam's signature mentions in its old shape and leaves everything else to Scalarize.
    class Keep : public Rewriter {
    public:
        Keep(Scalarize& sca)
            : Rewriter(sca.new_world())
            , sca_(sca) {}

        const Def* rewrite(const Def*) final;

    private:
        bool has_cn(const Def*);

        Scalarize& sca_;
        DefMap<bool> has_cn_;
    };

    const Def* rewrite_imm_Pi(const Pi*) final;
    /// The new Lam for @p old; the old Var maps to its reassembly from the new one.
    Lam* stub(Lam* old, const fe::Bitset& mask);
    /// Flattens @p pi%'s domain one level according to @p mask and rewrites its components with @p rw.
    const Def* flatten(Rewriter& rw, const Pi* pi, const fe::Bitset& mask);
    const Def* rewrite_mut_Lam(Lam*) final;

    Analysis analysis_;
};

} // namespace mim
