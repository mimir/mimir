#pragma once

#include <array>
#include <string_view>
#include <utility>

#include <mim/util/types.h>

namespace mim {
class Def;
class World;
} // namespace mim

/// The closed rewrite rules of the `x86_isel` plugin as C++; `x86_isel.gen` writes them to `isel_rules.h`.
namespace mim::plug::x86::rules {

#define MIM_X86_RULE_WANTS(X) X(Param) X(Lit) X(Reg) X(Imm) X(Sh) X(RegImm) X(RegSh) X(Base) X(Index) X(Addr) X(Scaled)

/// What a rule wants of the term a meta variable matches; a Param is bound but no operand.
/// A Lit is a Param that must be a literal.
/// Reg is a register, Base/Index an `x86.Base`/`x86.Index`, Addr an `x86.Addr i` for any `i`, and Scaled an index with
/// its scale. Imm is a literal that fits a sign-extended `i32` immediate, Sh a literal shift count, which x86 masks.
/// RegImm and RegSh are a Reg, or an Imm and a Sh for a literal that is one.
enum class Want : u8 {
#define X(w) w,
    MIM_X86_RULE_WANTS(X)
#undef X
};

#define MIM_X86_RULE_TYS(X) X(Reg) X(Base) X(Index) X(Scaled) X(Lit)

/// The non-terminal a rule yields, by the type of its rhs: a register, `x86.Base`, `x86.Index`, or an index with its
/// scale `[I64, I2]`. Lit is no rule's type but that of a literal operand.
enum class Ty : u8 {
#define X(t) t,
    MIM_X86_RULE_TYS(X)
#undef X
};

/// Matches the root @p def with the commuting nodes in @p swap swapped and binds @p metas and @p refs.
using MatchFn = bool (*)(const Def* def, u32 swap, const Def** metas, const Def** refs);
/// Builds the rhs from @p holes, one per meta variable, already in the new world.
using BuildFn = const Def* (*)(World&, const Def* const* holes);

/// How the lhs refers to one of its inner nodes.
enum class Ref : u8 {
    Value, ///< As an operand of its owner, which nothing outside the match uses.
    Mem,   ///< By its memory, which only the match uses.
};

/// A ref of a matcher: its cover and how the lhs refers to it.
struct RefInfo {
    u8 cover;
    Ref kind;
};

/// An operand position of a matcher in preorder: meta variable or Value ref @p index of the node @p owner.
struct Use {
    u8 owner;
    u8 index;
    bool ref;
};

/// A matcher and the shape of its lhs; a cover's operands follow it in preorder.
struct Matcher {
    MatchFn fn;
    u32 uses;
    u8 num_uses;
    u32 refs;
    u8 num_refs;
    u8 num_covers;
};

/// A closed rule `lhs ↦ rhs`, whose metas want `num_metas` consecutive entries of `wants`.
struct Rewrite {
    std::string_view name;
    u32 cost;     ///< The number of x86 instructions in its rhs.
    flags_t root; ///< The axiom at the head of its lhs, or 0 for an Extract.
    Ty type;      ///< The non-terminal its rhs yields.
    const Matcher* match;
    BuildFn build;
    u32 wants;
    u8 num_metas;
    u32 swaps;     ///< The swaps of `match` that may yield another match.
    u32 imm_swaps; ///< Those of `swaps` that only do if a RegImm or RegSh becomes an immediate.
    u8 maps;       ///< `k + 1` if the rhs replaces cover `k` as a whole, or 0.
};

/// The rewrites `rewrites[first, first + num)` with the root axiom @p flags; `roots` is sorted by it.
struct Root {
    flags_t flags;
    u32 first;
    u32 num;
};

/// A conversion rule `x ↦ rhs`, whose lhs is all of its meta variables: a value of non-terminal `from` as one of `to`.
/// Its builder takes the `arity` projections of the value.
struct Conversion {
    std::string_view name;
    u32 cost;
    Ty from, to;
    u8 arity;
    BuildFn build;
};

/// @name Costs
/// Shared by `x86_isel.gen`, which checks the rule set with them, and isel.
///@{
inline constexpr u32 Inf = u32(-1);

#define X(_) +1
inline constexpr size_t Num_Tys = 0 MIM_X86_RULE_TYS(X);
#undef X

constexpr u32 add(u32 a, u32 b) { return a == Inf || b == Inf ? Inf : a + b; }

/// Is a meta variable that @p w wants bound to its rewrite rather than an operand?
constexpr bool is_param(Want w) { return w == Want::Param || w == Want::Lit; }

/// Does a hole of type @p want take a literal rather than a replacement?
constexpr bool is_literal(Want want) { return want == Want::Imm || want == Want::Sh; }

/// The literal form of a hole of type @p want, or @p want itself.
constexpr Want literal_of(Want want) {
    return want == Want::RegImm ? Want::Imm : want == Want::RegSh ? Want::Sh : want;
}

/// Does a value of non-terminal @p ty fill a hole of type @p want as it is?
constexpr bool fills(Ty ty, Want want) {
    switch (want) {
        case Want::Reg:
        case Want::RegImm:
        case Want::RegSh: return ty == Ty::Reg;
        case Want::Base: return ty == Ty::Base;
        case Want::Index: return ty == Ty::Index;
        case Want::Addr: return ty == Ty::Base || ty == Ty::Index;
        case Want::Scaled: return ty == Ty::Scaled;
        default: return false;
    }
}

/// The cheapest chains of conversions between two non-terminals: their costs and each one's first conversion.
struct Chains {
    std::array<std::array<u32, Num_Tys>, Num_Tys> cost{};
    std::array<std::array<u32, Num_Tys>, Num_Tys> first{};

    /// The closure of @p conversions, a range of Conversion-like rules with `from`, `to`, and `cost`.
    constexpr Chains(const auto& conversions) {
        for (size_t i = 0; i != Num_Tys; ++i)
            for (size_t j = 0; j != Num_Tys; ++j)
                cost[i][j] = i == j ? 0 : Inf, first[i][j] = Inf;
        u32 n = 0;
        for (const auto& c : conversions) {
            if (auto& d = cost[size_t(c.from)][size_t(c.to)]; u32(c.cost) < d)
                d = u32(c.cost), first[size_t(c.from)][size_t(c.to)] = n;
            ++n;
        }
        for (size_t k = 0; k != Num_Tys; ++k)
            for (size_t i = 0; i != Num_Tys; ++i)
                for (size_t j = 0; j != Num_Tys; ++j)
                    if (auto c = add(cost[i][k], cost[k][j]); c < cost[i][j]) cost[i][j] = c, first[i][j] = first[i][k];
    }

    /// The non-terminal that @p from converts to most cheaply for a hole of type @p want, and the cost.
    constexpr std::pair<Ty, u32> target(Ty from, Want want) const {
        auto res = std::pair(from, Inf);
        for (size_t t = 0; t != Num_Tys; ++t)
            if (auto c = cost[size_t(from)][t]; fills(Ty(t), want) && c < res.second) res = {Ty(t), c};
        return res;
    }

    /// Cost of converting a replacement or operand of non-terminal @p from into a hole of type @p want.
    constexpr u32 conv(Ty from, Want want) const {
        if (is_literal(want)) return from == Ty::Lit ? 0 : Inf;
        return target(from, want).second;
    }

    /// Does every non-terminal convert into a hole of type @p w, as an atomic rule's holes must?
    constexpr bool total(Want w) const {
        for (size_t t = 0; t != Num_Tys; ++t)
            if (conv(Ty(t), w) == Inf) return false;
        return true;
    }

    /// Does some non-terminal of a rule convert into a hole of type @p w?
    constexpr bool reachable(Want w) const {
        for (size_t t = 0; t != Num_Tys; ++t)
            if (Ty(t) != Ty::Lit && conv(Ty(t), w) != Inf) return true;
        return false;
    }
};
///@}

} // namespace mim::plug::x86::rules
