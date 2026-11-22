#pragma once

#include <array>

#include <mim/def.h>
#include <mim/lam.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// Calls @p f for each block, possibly repeatedly, that the jump @p term passes control or a return continuation to.
void for_each_succ(const App* term, auto f) {
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
/// A branch on an `x86.cc` uses its flags; the `Bool` is a value only if something else uses it.
class Liveness {
public:
    enum Class : size_t { Gpr, Fpr, Flags, Stack, Num_Classes };
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

    explicit Liveness(Lam* fun);

    /// The entry comes first; the others follow in order of discovery.
    const auto& blocks() const { return blocks_; }
    const Block* block(Lam* lam) const {
        auto i = lam2block_.find(lam);
        return i == lam2block_.end() ? nullptr : &blocks_[i->second];
    }

    static Class cls(const Def* value);
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

    fe::Vector<Block> blocks_;
    LamMap<size_t> lam2block_;
    DefMap<std::pair<size_t, size_t>> insn2block_; ///< Block and position.
};

/// Is @p value a value of class @p cls?
inline bool is_value(const Def* value, Liveness::Class cls) {
    return !is_mem(value) && !value->type()->isa<Pi>() && Liveness::cls(value) == cls;
}

/// System V AMD64 ABI per register class: the XMM registers are their own argument registers and all caller-saved.
inline size_t num_arg_regs(Liveness::Class cls) { return cls == Liveness::Gpr ? Arg_Regs.size() : 8; }
inline size_t arg_reg(Liveness::Class cls, size_t i) { return cls == Liveness::Gpr ? Arg_Regs[i] : i; }
inline size_t ret_reg(Liveness::Class cls) { return cls == Liveness::Gpr ? encoding(reg::rax) : 0; }
inline bool caller_saved(Liveness::Class cls, size_t r) { return cls != Liveness::Gpr || Caller_Saved[r]; }

} // namespace mim::plug::x86
