#pragma once

#include <memory>

#include "mim/check.h"
#include "mim/def.h"
#include "mim/lam.h"
#include "mim/lattice.h"
#include "mim/rule.h"
#include "mim/tuple.h"

namespace mim {

class World;

/// Recurseivly rebuilds part of a program **into** the provided World w.r.t.\ Rewriter::map.
/// This World may be different than the World we started with.
/// @see @ref rewriter
class Rewriter {
public:
    /// @name Construction & Destruction
    ///@{
    Rewriter(std::unique_ptr<World>&& ptr);
    Rewriter(World& world);
    virtual ~Rewriter();

    void reset(std::unique_ptr<World>&& ptr);
    void reset();
    ///@}

    /// @name Getters
    ///@{
    World& world() { return *world_; }
    ///@}

    /// @name Push / Pop
    ///@{
    virtual void push() { old2news_.emplace_back(Def2Def{}); }
    virtual void pop() { old2news_.pop_back(); }
    ///@}

    /// @name Map / Lookup
    /// Map @p old_def to @p new_def and returns @p new_def.
    ///@{
    virtual const Def* map(const Def* old_def, const Def* new_def) { return old2news_.back()[old_def] = new_def; }

    // clang-format off
    const Def* map(const Def* old_def ,       Defs new_defs);
    const Def* map(Defs       old_defs, const Def* new_def );
    const Def* map(Defs       old_defs,       Defs new_defs);
    // clang-format on

    /// Lookup `old_def` by searching in reverse through the stack of maps.
    /// @returns `nullptr` if nothing was found.
    virtual const Def* lookup(const Def* old_def) {
        for (const auto& old2new : old2news_ | std::views::reverse)
            if (auto i = old2new.find(old_def); i != old2new.end()) return i->second;
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

    friend void swap(Rewriter& rw1, Rewriter& rw2) noexcept {
        using std::swap;
        swap(rw1.old2news_, rw2.old2news_);
        // Do NOT swap ptr_ and world_: they are back pointers!
    }

private:
    std::unique_ptr<World> ptr_;
    World* world_;

protected:
    std::deque<Def2Def> old2news_;
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

    // Add initial mapping from @pvar -> @p arg.
    VarRewriter& add(const Var* var, const Def* arg) {
        map(var, arg);
        vars_.emplace_back(var);
        return *this;
    }
    ///@}

    /// @name push / pop
    ///@{
    void push() final { Rewriter::push(), vars_.emplace_back(Vars()); }
    void pop() final { vars_.pop_back(), Rewriter::pop(); }
    ///@}

    /// @name rewrite
    ///@{
    const Def* rewrite(const Def*) final;
    const Def* rewrite_mut(Def*) final;
    ///@}

    friend void swap(VarRewriter& vrw1, VarRewriter& vrw2) noexcept {
        using std::swap;
        swap(static_cast<Rewriter&>(vrw1), static_cast<Rewriter&>(vrw2));
        swap(vrw1.vars_, vrw2.vars_);
    }

private:
    bool has_intersection(const Def* old_def) {
        for (const auto& vars : vars_ | std::views::reverse)
            if (vars.has_intersection(old_def->free_vars())) return true;
        return false;
    }

    Vector<Vars> vars_;
};

class Zonker : public Rewriter {
public:
    /// @name C'tor
    ///@{
    Zonker(World& world)
        : Rewriter(world) {}
    ///@}

    /// @name Stack of Maps
    ///@{
    const Def* map(const Def* old_def, const Def* new_def) final;
    const Def* lookup(const Def* old_def) final;
    ///@}

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

private:
    const Def* get(const Def* old_def) {
        auto& old2new = old2news_.back();
        if (auto i = old2new.find(old_def); i != old2new.end()) return i->second;
        return nullptr;
    }
};

} // namespace mim
