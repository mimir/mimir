#pragma once

#include <algorithm>
#include <array>
#include <ranges>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/rewrite.h>

#include "mim/plug/mach/target.h"

namespace mim::plug::mach {

/// Calls @p f for each block, possibly repeatedly, that the jump @p term passes control or a return continuation to.
inline void for_each_succ(const App* term, auto f) {
    auto succ = [&](const Def* def) {
        if (auto bb = Lam::isa_mut_basicblock(def); bb && bb->is_set()) f(bb);
    };
    succ(term->callee());
    if (auto ex = term->callee()->isa<Extract>())
        for (auto target : ex->tuple()->projs())
            succ(target);
    for (auto arg : term->args())
        succ(arg);
}

/// Liveness of the values of one scheduled function, as a side table.
/// A *value* is a non-memory result of an instruction on a `mem.M 0` chain or a non-memory parameter.
/// A parameter is defined at the entry of its block and a jump uses its arguments, so a parameter is never live-in.
/// A branch on a condition code uses its flags; the `Bool` is a value only if something else uses it.
class Liveness {
public:
    using Class = Target::Class;
    using enum Target::Class;
    using Pressure = std::array<size_t, Num_Classes>;

    struct Block {
        Lam* lam;
        fe::Vector<const App*> insns;  ///< The `mem.M 0` chain in execution order.
        fe::Vector<DefVec> defs, uses; ///< Per instruction.
        DefVec params, term_uses;
        fe::Vector<size_t> preds, succs;
        DefSet live_in, live_out;
        Pressure max_live = {}; ///< Maximum over all program points, per Class; Stack is unbounded.
    };

    Liveness(const Target& target, Lam* fun);

    /// The entry comes first; the others follow in order of discovery.
    const auto& blocks() const { return blocks_; }
    const Block* block(Lam* lam) const {
        auto i = lam2block_.find(lam);
        return i == lam2block_.end() ? nullptr : &blocks_[i->second];
    }

    const Target& target() const { return *target_; }
    /// Is @p def a parameter of a block or a result of an instruction on a chain, or a projection thereof?
    bool local(const Def* def) const;
    const DefVec& params(const Block& block) const { return block.params; }
    const DefVec& defs(const App* insn) const { return at(insn, &Block::defs); }
    const DefVec& uses(const App* insn) const { return at(insn, &Block::uses); }
    /// The values the jump at the end reads.
    const DefVec& term_uses(const Block& block) const { return block.term_uses; }
    const Block& def_block(const Def* value) const;

private:
    size_t add(Lam*);
    void chain(size_t);
    void values(const Def*, DefVec&, DefSet&) const;
    void values(Block&);
    const DefVec& at(const App* insn, fe::Vector<DefVec> Block::* vecs) const {
        auto [b, i] = insn2block_.find(insn)->second;
        return (blocks_[b].*vecs)[i];
    }
    void live_in(size_t, const Def*);
    void sweep(Block&);

    const Target* target_;
    fe::Vector<Block> blocks_;
    LamMap<size_t> lam2block_;
    DefMap<std::pair<size_t, size_t>> insn2block_; ///< Block and position.
};

/// Is @p value a value of class @p cls?
inline bool is_value(const Def* value, Liveness::Class cls, const Target& target) {
    return !target.is_mem(value) && !value->type()->isa<Pi>() && target.cls(value) == cls;
}

/// Does a jump to @p lam pass values, which a branch cannot do on its own?
bool takes_values(Lam* lam);

/// The blocks of @p live in emission order: each block is followed by the one it falls through to, where possible.
fe::Vector<const Liveness::Block*> layout(const Liveness& live);

/// Calls @p f for each use of a value in @p live.
void for_each_use(const Liveness& live, auto f) {
    for (const auto& block : live.blocks()) {
        for (const auto& uses : block.uses)
            for (auto u : uses)
                f(u);
        for (auto u : block.term_uses)
            f(u);
    }
}

/// Rewrites the filter and body of each block of @p live in place.
void rewrite_blocks(const Liveness& live, Rewriter& rw);

/// Wraps every use of a value that @p regs assigns a register in `in`.
class Assign : public Rewriter {
public:
    Assign(World& world, Target::Class cls, const Liveness& live, const DefMap<size_t>& regs)
        : Rewriter(world)
        , cls_(cls)
        , live_(live)
        , regs_(regs) {}

    const Def* rewrite(const Def*) final;

private:
    /// @p def, a parameter, an instruction, or a projection thereof, without wrapping it.
    const Def* raw(const Def* def);

    Target::Class cls_;
    const Liveness& live_;
    const DefMap<size_t>& regs_;
};

/// The block that a call at the end of @p block returns to, if any.
inline const Liveness::Block* call_cont(const Liveness& live, Lam* fun, const Liveness::Block& block) {
    auto term = block.lam->body()->as<App>();
    if (term->callee() == fun->ret_var() || !Lam::isa_mut_returning(term->callee())) return nullptr;
    if (auto k = Lam::isa_mut_basicblock(term->arg(term->num_args(), term->num_args() - 1))) return live.block(k);
    return nullptr;
}

/// Calls @p f `(d, v)` for each value `v` that is live at the definition of `d` or defined along with it.
/// Only values that satisfy @p keep take part.
template<class Keep, class F>
void interferences(const Liveness& live, const Liveness::Block& block, Keep keep, F f) {
    auto alive = DefSet();
    auto gen   = [&](const auto& values) {
        for (auto v : values)
            if (keep(v)) alive.emplace(v);
    };
    auto def = [&](DefVec ds) {
        std::erase_if(ds, [&](const Def* d) { return !keep(d); });
        for (auto d : ds)
            alive.erase(d);
        for (size_t i = 0; i != ds.size(); ++i) {
            for (auto v : alive)
                f(ds[i], v);
            for (size_t j = i + 1; j != ds.size(); ++j)
                f(ds[i], ds[j]);
        }
    };

    gen(block.live_out);
    gen(live.term_uses(block));
    for (auto insn : block.insns | std::views::reverse) {
        def(live.defs(insn));
        gen(live.uses(insn));
    }
    def(live.params(block));
}

} // namespace mim::plug::mach
