#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::autodiff {

/// This phase is the heart of AD.
/// We replace an `autodiff fun` call with the differentiated function.
class Eval : public RWPhase {
public:
    Eval(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    /// Detect autodiff calls.
    const Def* rewrite_imm_App(const App*) final;

    /// Acts on toplevel autodiff on closed terms:
    /// * Replaces lambdas, operators with the appropriate derivatives.
    /// * Creates new lambda, calls associate variables, init maps, calls augment.
    const Def* derive(const Def*);
    const Def* derive_(const Def*);

    /// Applies to (open) expressions in a functional context.
    /// Returns the rewritten expressions and augments the partial and modular pullbacks.
    /// The rewrite is identity on the term up to renaming of variables.
    /// Otherwise, only pullbacks are added.
    /// To do so, some calls (e.g. axms) are replaced by their derivatives.
    /// This transformation can be seen as an augmentation with a dual computation that generates the derivatives.
    const Def* augment(const Def*, Lam*, Lam*);
    const Def* augment_(const Def*, Lam*, Lam*);
    /// helper functions for augment
    const Def* augment_var(const Var*, Lam*, Lam*);
    const Def* augment_lam(Lam*, Lam*, Lam*);
    const Def* augment_extract(const Extract*, Lam*, Lam*);
    const Def* augment_app(const App*, Lam*, Lam*);
    const Def* augment_lit(const Lit*, Lam*, Lam*);
    const Def* augment_tuple(const Tuple*, Lam*, Lam*);
    const Def* augment_pack(const Pack* pack, Lam* f, Lam* f_diff);

private:
    /// derive()/augment() run entirely in the new world: the `%autodiff.ad` argument is rewritten first, and the
    /// derivative is built from that copy. This alias resolves the world() calls in the augment machinery accordingly.
    World& world() { return new_world(); }

    /// Transforms closed terms (lambda, operator) to derived expressions.
    /// `f => f' = λ x. (f x, f*_x)`
    /// src Def -> dst Def
    Def2Def derived;
    /// Associates expressions (not necessarily closed) in a functional context to their derivatived counterpart.
    /// src Def -> dst Def
    Def2Def augmented;

    /// dst Def -> dst Def
    Def2Def partial_pullback;

    /// Shadows the structure of containers for additional auxiliary pullbacks.
    /// A very advanced optimization might be able to recover shadow pullbacks from the partial pullbacks.
    /// Example: The shadow pullback of a tuple is a tuple of pullbacks.
    /// Shadow pullbacks are not modular (composable with other pullbacks).
    /// The structure pullback only preserves structure shallowly:
    /// A n-times nested tuple has a tuple of "normal" pullbacks as shadow pullback.
    /// Each inner nested tuples should have their own structure pullback by construction.
    /// ```
    /// e  : [B0, [B11, B12], B2]
    /// e* : [B0, [B11, B12], B2] -> A
    /// e*S: [B0 -> A, [B11, B12] -> A, B2 -> A]
    /// ```
    /// short theory of shadow pb:
    /// ```
    /// t: [B0, ..., Bn]
    /// t*: [B0, ..., Bn] -> A
    /// t*_S: [B0 -> A, ..., Bn -> A]
    /// b = t#i : Bi
    /// b* : Bi -> A
    /// b* = t*_S #i (if exists)
    /// ```
    /// This is equivalent to:
    /// `\lambda (s:Bi). t*_S (insert s at i in (zero [B0, ..., Bn]))`
    /// dst Def -> dst Def
    Def2Def shadow_pullback;
};

} // namespace mim::plug::autodiff
