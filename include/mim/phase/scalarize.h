#pragma once

#include <fe/bitset.h>

#include "mim/phase.h"

#include "mim/util/gid.h"

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
/// An Axm handed a bare function (`autodiff.ad f`) is instantiated with the old shape, as its type may rely on it.
/// A Pi is pinned where a coercion would be observable:
/// * a `Cn` an interface Lam's signature reaches through a type coerce() cannot take apart (`mem.Ptr0 K`), or
/// * a parameter indexed non-constantly or consumed whole by an Axm - only this parameter is kept.
///
/// Each run flattens one level; the fixed-point pipeline repeats it.
/// Flags::scalarize_threshold bounds which Arr%s are expanded.
class Scalarize : public RWPhase {
private:
    /// Optimistic: every immutable `Cn` is flattenable until pinned.
    class Analysis : public mim::Analysis {
    public:
        Analysis(World& world)
            : mim::Analysis(world, "Scalarize::Analysis") {}

        /// The parameters of @p type to flatten one level; empty means leave it untouched.
        fe::Bitset plan(const Def* type) const;

    private:
        const Def* rewrite_mut_Lam(Lam*) final;
        const Def* rewrite_imm_App(const App*) final;
        const Def* rewrite_imm_Extract(const Extract*) final;
        const Def* rewrite_imm_Insert(const Insert*) final;

        /// Keeps @p tuple whole unless the outermost part of @p idx is constant.
        void keep_indexed(const Def* tuple, const Def* idx);
        /// Keeps parameter @p dom of @p pi whole.
        void keep(const Pi* pi, size_t dom);
        /// Keeps @p def whole if it is a Lam's Var or one of its parameters.
        void keep_var(const Def* def);
        bool kept(const Pi* pi, size_t dom) const; ///< Is parameter @p dom of @p pi kept whole?
        /// Does coerce() see through @p type, i.e. split it or η-wrap it?
        /// The shape of whatever lies below any other type must be kept.
        static bool transparent(const Def* type) { return type->isa<Pi>() || splittable(type); }
        /// pin%s the `Cn`%s below the parts of @p type that coerce() cannot take apart.
        void pin_opaque(DefSet& structural, const Def* type);

        /// Not in lattice(), as its map() of the Pi would break type arguments like `mem.slot (H, 0)`.
        GIDMap<const Pi*, fe::Bitset> keeps_;
    };

public:
    Scalarize(World& world, flags_t annex)
        : RWPhase(world, annex, &analysis_)
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
    const Def* rewrite_imm_App(const App*) final;
    /// The new Lam for @p old; the old Var maps to its reassembly from the new one.
    Lam* stub(Lam* old, const fe::Bitset& mask);
    /// Flattens @p pi%'s domain one level according to @p mask and rewrites its components with @p rw.
    const Def* flatten(Rewriter& rw, const Pi* pi, const fe::Bitset& mask);
    const Def* rewrite_mut_Lam(Lam*) final;

    Analysis analysis_;
};

} // namespace mim
