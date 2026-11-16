#pragma once

#include <array>

#include <mim/def.h>
#include <mim/lam.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// Liveness of the values of one scheduled function, as a side table.
/// A *value* is a non-memory result of an instruction on a `mem.M 0` chain or a non-memory parameter.
/// A parameter is defined at the entry of its block and a jump uses its arguments, so a parameter is never live-in.
/// A branch on an `x86.cc` uses its flags; the `Bool` is a value only if something else uses it.
class Liveness {
public:
    enum Class : size_t { Gpr, Flags, Stack, Num_Classes };
    using Pressure = std::array<size_t, Num_Classes>;

    struct Block {
        Lam* lam;
        fe::Vector<const App*> insns; ///< The `mem.M 0` chain in execution order.
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
    DefVec params(const Block&) const;
    DefVec defs(const App* insn) const;
    DefVec uses(const App* insn) const;
    DefVec term_uses(const Block&) const; ///< The values the jump at the end reads.
    const Block& def_block(const Def* value) const;

private:
    size_t add(Lam*);
    void chain(size_t);
    void values(const Def*, DefVec&, DefSet&) const;
    void live_in(size_t, const Def*);
    void sweep(Block&);

    fe::Vector<Block> blocks_;
    LamMap<size_t> lam2block_;
    DefMap<size_t> insn2block_;
};

/// Is @p value a value of class Liveness::Gpr?
inline bool is_gpr(const Def* value) {
    return !is_mem(value) && !value->type()->isa<Pi>() && Liveness::cls(value) == Liveness::Gpr;
}

} // namespace mim::plug::x86
