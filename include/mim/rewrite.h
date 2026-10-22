#pragma once

#include <fe/restore.h>

#include "mim/check.h"
#include "mim/def.h"
#include "mim/lam.h"
#include "mim/nom.h"
#include "mim/rule.h"
#include "mim/single.h"
#include "mim/tuple.h"
#include "mim/union.h"

namespace mim {

class World;

/// Recurseivly rebuilds part of a program **into** the provided World w.r.t.\ Rewriter::map.
/// This World may be different than the World we started with.
/// @see @ref rewriter
class Rewriter {
public:
    /// @name Construction & Destruction
    ///@{
    Rewriter(World& world);
    virtual ~Rewriter();

    void reset();
    ///@}

    /// @name Getters
    ///@{
    World& world() { return *world_; }
    ///@}

    /// @name Scope
    ///@{
    /// Opens a scope for the mappings whose old Def depends on @p old_vars or whose new Def depends on @p new_vars.
    /// The root scope takes all others.
    void push(Vars old_vars = {}, Vars new_vars = {}) { scopes_.emplace_back(old_vars, new_vars); }
    void pop() { scopes_.pop_back(); }
    /// Adds the old @p var - if any - to the innermost scope.
    void bind_old(const Var* var);
    /// Adds the new @p var - if any - to the innermost scope.
    void bind_new(const Var* var);
    void bind(const Var* old_var, const Var* new_var) {
        bind_old(old_var);
        bind_new(new_var);
    }
    ///@}

    /// @name Map / Lookup
    /// Map @p old_def to @p new_def and returns @p new_def.
    ///@{
    /// Records the mapping in the innermost push() scope it depends on.
    const Def* map(const Def* old_def, const Def* new_def);

    // clang-format off
    const Def* map(const Def* old_def ,       Defs new_defs);
    const Def* map(Defs       old_defs, const Def* new_def );
    const Def* map(Defs       old_defs,       Defs new_defs);
    // clang-format on

    /// Lookup `old_def` by searching in reverse through the stack of maps.
    /// @returns `nullptr` if nothing was found.
    virtual const Def* lookup(const Def* old_def) {
        for (const auto& scope : scopes_ | std::views::reverse)
            if (auto new_def = fe::lookup(scope.old2new, old_def)) return new_def;
        return nullptr;
    }
    ///@}

    /// @name rewrite
    /// Recursively rewrite old Def%s.
    ///@{
    virtual const Def* rewrite(const Def*);
    virtual const Def* rewrite_imm(const Def*);
    virtual const Def* rewrite_mut(Def*);
    virtual const Def* rewrite_stub(Def*, Def*);
    /// Like above, but @p new_mut may have a *different* number of ops than @p old_mut:
    /// `new_mut->op(i)` is rewritten from `old_mut->op(new2old[i])`.
    /// This is what a Phase needs that reshapes an aggregate; @see Sieve.
    const Def* rewrite_stub(Def* old_mut, Def* new_mut, fe::View<size_t> new2old);
    virtual DefVec rewrite(Defs);

#define CODE_IMM(N) virtual const Def* rewrite_imm_##N(const N*);
#define CODE_MUT(N) virtual const Def* rewrite_mut_##N(N*);
    MIM_IMM_NODE(CODE_IMM)
    MIM_MUT_NODE(CODE_MUT)
#undef CODE_IMM
#undef CODE_MUT

    virtual const Def* rewrite_imm_Seq(const Seq* seq);
    virtual const Def* rewrite_mut_Seq(Seq* seq);
    ///@}

    template<class D = Def>
    D* curr_mut() const {
        return curr_mut_ ? curr_mut_->template isa<D>() : nullptr;
    }

protected:
    /// The map of the innermost push() scope.
    Def2Def& old2new() { return scopes_.back().old2new; }

    /// Does @p old_def depend on the old Vars of any scope?
    bool has_intersection(const Def* old_def) {
        // Def::has_free_vars_in avoids materializing old_def's merged free_vars() - once per level, at that.
        for (const auto& scope : scopes_ | std::views::reverse)
            if (old_def->has_free_vars_in(scope.old_vars)) return true;
        return false;
    }

    /// Updates curr_mut() to @p new_mut and restores it at the end of the scope.
    [[nodiscard]] auto enter(Def* new_mut) { return fe::Restore(curr_mut_, new_mut); }

    /// The tail of rewrite_stub: immutabilizes @p new_mut in hindsight, as rewriting may have made it vacuous.
    /// A stub that already has users stays mutable, as the users captured it during its own rewrite.
    /// Only needed if you fill a stub yourself instead of going through rewrite_stub.
    const Def* seal(Def* old_mut, Def* new_mut);

    struct Scope {
        Vars old_vars; ///< Only mappings depending on these go here; the root's are VarRewriter's substitutions.
        Vars new_vars;
        Def2Def old2new;
    };
    fe::Vector<Scope> scopes_;

private:
    World* world_;
    Def* curr_mut_ = nullptr;

    friend void swap(Rewriter& rw1, Rewriter& rw2) noexcept {
        using std::swap;
        swap(rw1.scopes_, rw2.scopes_);
        // Do NOT swap world_: it is a back pointer!
    }
};

/// Extends Rewriter for variable substitution.
/// @see @ref rewriter
class VarRewriter : public Rewriter {
public:
    /// @name Construction
    ///@{
    VarRewriter(World& world)
        : Rewriter(world) {}
    VarRewriter(const Var* var, const Def* arg)
        : Rewriter(arg->world()) {
        add(var, arg);
    }

    /// Substitutes @p arg for @p var.
    VarRewriter& add(const Var* var, const Def* arg) {
        map(var, arg);
        bind_old(var);
        return *this;
    }
    ///@}

    /// @name rewrite
    ///@{
    const Def* rewrite(const Def*) final;
    const Def* rewrite_mut(Def*) final;
    ///@}

    friend void swap(VarRewriter& vrw1, VarRewriter& vrw2) noexcept {
        using std::swap;
        swap(static_cast<Rewriter&>(vrw1), static_cast<Rewriter&>(vrw2));
    }
};

class Zonker : public Rewriter {
public:
    /// @name C'tor
    ///@{
    Zonker(World& world)
        : Rewriter(world) {}
    ///@}

    const Def* lookup(const Def* old_def) final;

    /// @name rewrite
    ///@{
    const Def* rewrite(const Def*) final;
    const Def* rewrite_mut(Def* mut) final { return map(mut, mut); }
    const Def* rewire_mut(Def*);
    ///@}

    friend void swap(Zonker& z1, Zonker& z2) noexcept {
        using std::swap;
        swap(static_cast<Rewriter&>(z1), static_cast<Rewriter&>(z2));
    }
};

/// Imports from Driver::library: an annex therein is looked up via World::annex, so it is imported only once.
class Importer : public Rewriter {
public:
    Importer(World& world)
        : Rewriter(world) {}

    const Def* import(const Def* root) { return Rewriter::rewrite(root); }
    const Def* rewrite(const Def*) final;

    friend void swap(Importer& i1, Importer& i2) noexcept {
        using std::swap;
        swap(static_cast<Rewriter&>(i1), static_cast<Rewriter&>(i2));
    }
};

} // namespace mim
