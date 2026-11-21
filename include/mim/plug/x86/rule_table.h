#pragma once

#include <string_view>

#include <mim/util/types.h>

/// The closed rewrite rules of the `x86_isel` plugin as data; `x86_isel.gen` writes them to `isel_rules.h`.
namespace mim::plug::x86::rules {

#define MIM_X86_RULE_KINDS(X) X(Hole) X(Axm) X(Lit) X(Nat) X(Idx) X(App) X(Tuple) X(Pack) X(Extract) X(Sigma) X(Arr)

enum class Kind : u8 {
#define X(k) k,
    MIM_X86_RULE_KINDS(X)
#undef X
};

/// A node of the term pool all rules share, after its operands.
struct Node {
    Kind kind;
    u8 num_ops = 0;
    u32 ops    = 0; ///< The first of its operands in `ops`.
    u64 val    = 0; ///< The flags of an Axm, the value of a Lit (its operand is its type), or the index of a Hole.
};

#define MIM_X86_RULE_WANTS(X) X(Param) X(Reg) X(Imm) X(Sh) X(Base) X(Index) X(Addr)

/// What a rule wants of the term a meta variable matches; a Param is bound but no operand.
enum class Want : u8 {
#define X(w) w,
    MIM_X86_RULE_WANTS(X)
#undef X
};

struct Meta {
    Want want;
    u32 type; ///< Its type in the node pool.
};

/// A closed rule `lhs ↦ rhs`, whose metas are `num_metas` consecutive entries of `metas`.
struct Rewrite {
    std::string_view name;
    u32 cost; ///< The number of x86 instructions in `rhs`.
    u32 lhs;
    u32 rhs;
    u32 metas;
    u8 num_metas;
};

} // namespace mim::plug::x86::rules
