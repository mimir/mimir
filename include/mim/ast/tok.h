#pragma once

#include <fe/assert.h>
#include <fe/format.h>

#include "mim/util/dbg.h"

namespace mim {

class Def;
class Lit;

namespace ast {

/// @name Precedence Table
/// X-macro listing all expression precedences from lowest to highest.
/// Each entry is `m(name, assoc)` where @p assoc is `L`, `R`, or `N`.
///@{
// clang-format off
#define MIM_PREC(m)     \
    m(Err,     N)       \
    m(Bot,     N)       \
    m(Where,   L)       \
    m(Arrow,   R)       \
    m(Pi,      N)       \
    m(Inj,     R)       \
    m(App,     L)       \
    m(Union,   L)       \
    m(Extract, L)       \
    m(Lit,     N)
// clang-format on
///@}

/// Associativity of an infix expression.
enum class Assoc { N, L, R };

/// Expression precedences used by the parser and the dumper; ordered low to high.
enum class Prec {
#define CODE(name, ...) name,
    MIM_PREC(CODE)
#undef CODE
};

/// Associativity of precedence level @p p.
constexpr Assoc prec_assoc(Prec p) {
    switch (p) {
#define CODE(name, assoc) \
    case Prec::name: return Assoc::assoc;
        MIM_PREC(CODE)
#undef CODE
    }
    fe::unreachable();
}

constexpr bool is_rassoc(Prec p) { return prec_assoc(p) == Assoc::R; }
constexpr bool is_lassoc(Prec p) { return prec_assoc(p) == Assoc::L; }

/// Should a Pratt parser reduce when the current binding power is @p curr
/// and the infix operator has precedence @p op?
constexpr bool should_reduce(Prec curr, Prec op) { return is_rassoc(op) ? curr > op : curr >= op; }

// clang-format off
#define MIM_KEY(m)                    \
    m(K_Bool,   "Bool"  )             \
    m(K_Cn,     "Cn"    )             \
    m(K_Fn,     "Fn"    )             \
    m(K_I1,     "I1"    )             \
    m(K_I16,    "I16"   )             \
    m(K_I32,    "I32"   )             \
    m(K_I64,    "I64"   )             \
    m(K_I8,     "I8"    )             \
    m(K_Idx,    "Idx"   )             \
    m(K_Nat,    "Nat"   )             \
    m(K_Type,   "Type"  )             \
    m(K_Univ,   "Univ"  )             \
    m(K_and,    "and"   )             \
    m(K_as,     "as"    )             \
    m(K_axm,    "axm"   )             \
    m(K_ccon,   "ccon"  )             \
    m(K_cfun,   "cfun"  )             \
    m(K_cn,     "cn"    )             \
    m(K_con,    "con"   )             \
    m(K_end,    "end"   )             \
    m(K_extern, "extern")             \
    m(K_ff,     "ff"    )             \
    m(K_fn,     "fn"    )             \
    m(K_fun,    "fun"   )             \
    m(K_i1,     "i1"    )             \
    m(K_i16,    "i16"   )             \
    m(K_i32,    "i32"   )             \
    m(K_i64,    "i64"   )             \
    m(K_i8,     "i8"    )             \
    m(K_import, "import")             \
    m(K_inj,    "inj"   )             \
    m(K_ins,    "ins"   )             \
    m(K_lam,    "lam"   )             \
    m(K_let,    "let"   )             \
    m(K_match,  "match" )             \
    m(K_module, "module")             \
    m(K_norm,   "norm"  )             \
    m(K_plugin, "plugin")             \
    m(K_rec,    "rec"   )             \
    m(K_ret,    "ret"   )             \
    m(K_rule,   "rule"  )             \
    m(K_Rule,   "Rule"  )             \
    m(K_tt,     "tt"    )             \
    m(K_when,   "when"  )             \
    m(K_where,  "where" )             \
    m(K_with,   "with"  )             \

#define CODE(t, str) + size_t(1)
constexpr auto Num_Keys = size_t(0) MIM_KEY(CODE);
#undef CODE

#define MIM_TOK(m)                     \
    m(EoF, "<end of file>"    )        \
    /* literals */                     \
    m(L_s, "<signed integer literal>") \
    m(L_u, "<integer literal>"       ) \
    m(L_i, "<index literal>"         ) \
    m(L_f, "<floating-point literal>") \
    m(L_c, "<char literal>"          ) \
    m(L_str,  "<string literal>"     ) \
    /* misc */                         \
    m(M_id,   "<identifier>"  )        \
    m(M_anx,  "<annex name>"  )        \
    /* delimiters */                   \
    m(D_angle_l,    "‹")               \
    m(D_angle_r,    "›")               \
    m(D_brace_l,    "{")               \
    m(D_brace_r,    "}")               \
    m(D_brckt_l,    "[")               \
    m(D_brckt_r,    "]")               \
    m(D_curly_l,    "⦃")               \
    m(D_curly_r,    "⦄")               \
    m(D_paren_l,    "(")               \
    m(D_paren_r,    ")")               \
    m(D_quote_l,    "«")               \
    m(D_quote_r,    "»")               \
    /* further tokens */               \
    m(T_arrow,      "→")               \
    m(T_fat_arrow, "=>")               \
    m(T_assign,     "=")               \
    m(T_at,         "@")               \
    m(T_bot,        "⊥")               \
    m(T_top,        "⊤")               \
    m(T_box,        "□")               \
    m(T_colon,      ":")               \
    m(T_comma,      ",")               \
    m(T_dollar,     "$")               \
    m(T_dot,        ".")               \
    m(T_extract,    "#")               \
    m(T_lm,         "λ")               \
    m(T_semicolon,  ";")               \
    m(T_star,       "*")               \
    m(T_union,      "∪")               \
    m(T_pipe,       "|")               \

#define MIM_SUBST(m)                  \
    m("lm",     T_lm   )              \
    m("bot",    T_bot  )              \
    m("top",    T_top  )              \
    m("insert", K_ins  )              \

class Tok {
public:
    /// @name Tag
    ///@{
    enum class Tag {
        Nil,
#define CODE(t, str) t,
        MIM_KEY(CODE) MIM_TOK(CODE)
#undef CODE
    };

    static const char* tag2str(Tok::Tag);
    static constexpr Tok::Tag delim_l2r(Tag tag) { return Tok::Tag(int(tag) + 1); }
    ///@}

    // clang-format on

    Tok() {}
    Tok(Loc loc, Tag tag)
        : loc_(loc)
        , tag_(tag) {}
    Tok(Loc loc, char8_t c)
        : loc_(loc)
        , tag_(Tag::L_c)
        , c_(c) {}
    Tok(Loc loc, uint64_t u)
        : loc_(loc)
        , tag_(Tag::L_u)
        , u_(u) {}
    Tok(Loc loc, int64_t s)
        : loc_(loc)
        , tag_(Tag::L_s)
        , u_(std::bit_cast<uint64_t>(s)) {}
    Tok(Loc loc, double d)
        : loc_(loc)
        , tag_(Tag::L_f)
        , u_(std::bit_cast<uint64_t>(d)) {}
    Tok(Loc loc, const Lit* i)
        : loc_(loc)
        , tag_(Tag::L_i)
        , i_(i) {}
    Tok(Loc loc, Tag tag, Sym sym)
        : loc_(loc)
        , tag_(tag)
        , sym_(sym) {
        assert(tag == Tag::M_id || tag == Tag::M_anx || tag == Tag::L_str);
    }

    bool isa(Tag tag) const { return tag == tag_; }
    Tag tag() const { return tag_; }
    Dbg dbg() const { return {loc(), sym()}; }
    Loc loc() const { return loc_; }
    explicit operator bool() const { return tag_ != Tag::Nil; }
    // clang-format off
    const Lit* lit_i() const { assert(isa(Tag::L_i)); return i_; }
    char8_t    lit_c() const { assert(isa(Tag::L_c)); return c_;   }
    uint64_t   lit_u() const { assert(isa(Tag::L_u ) || isa(Tag::L_s ) || isa(Tag::L_f  )); return u_;   }
    Sym        sym()   const { assert(isa(Tag::M_anx) || isa(Tag::M_id) || isa(Tag::L_str)); return sym_; }
    // clang-format on
    std::string str() const;

    friend std::ostream& operator<<(std::ostream&, Tok);
    friend std::ostream& operator<<(std::ostream& os, Tok::Tag tag) { return os << tag2str(tag); }

private:
    Loc loc_;
    Tag tag_ = Tag::Nil;
    union {
        Sym sym_;
        uint64_t u_;
        char8_t c_;
        const Lit* i_;
    };
};

} // namespace ast
} // namespace mim

#ifndef DOXYGEN // clang-format off
template<> struct std::formatter<mim::ast::Tok     > : fe::ostream_formatter {};
template<> struct std::formatter<mim::ast::Tok::Tag> : fe::ostream_formatter {};
#endif // clang-format on
