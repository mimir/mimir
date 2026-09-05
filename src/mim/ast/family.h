#pragma once

/// @file
/// Families of Tok::Tag as reusable `case` labels; include this in `*.cpp` files only.
/// A family expands to `first: case Tag::second: ...` and hence must be used as `case Tag::C_FAMILY:`.
/// Families compose: a family may name another one.
/// The including file must provide `using Tag = Tok::Tag;`.

// clang-format off
#define C_PRIMARY     \
              K_Univ: \
    case Tag::K_Nat:  \
    case Tag::K_Idx:  \
    case Tag::K_Bool: \
    case Tag::K_ff:   \
    case Tag::K_tt:   \
    case Tag::K_i1:   \
    case Tag::K_i8:   \
    case Tag::K_i16:  \
    case Tag::K_i32:  \
    case Tag::K_i64:  \
    case Tag::K_I1:   \
    case Tag::K_I8:   \
    case Tag::K_I16:  \
    case Tag::K_I32:  \
    case Tag::K_I64:  \
    case Tag::T_star: \
    case Tag::T_box

#define C_ID         \
              M_anx: \
    case Tag::M_id

#define C_LIT        \
              T_bot: \
    case Tag::T_top: \
    case Tag::L_str: \
    case Tag::L_c:   \
    case Tag::L_s:   \
    case Tag::L_u:   \
    case Tag::L_f:   \
    case Tag::L_i

/// Literals that already determine their type and hence must not be ascribed one.
#define C_LIT_TYPED  \
              L_str: \
    case Tag::L_c:   \
    case Tag::L_i

#define C_LAM        \
              K_lam: \
    case Tag::K_con: \
    case Tag::K_fun

#define C_CDECL       \
              K_ccon: \
    case Tag::K_cfun

#define C_RULE        \
              K_norm: \
    case Tag::K_rule

#define C_IMPORT        \
              K_import: \
    case Tag::K_plugin

#define C_DECL          \
              K_axm:    \
    case Tag::K_let:    \
    case Tag::K_mod:    \
    case Tag::K_rec:    \
    case Tag::K_use:    \
    case Tag::C_CDECL:  \
    case Tag::C_IMPORT: \
    case Tag::C_RULE:   \
    case Tag::C_LAM

/// Direct-style binders; all other binders are CPS.
#define C_DS        \
              T_lm: \
    case Tag::K_lam

/// Binders whose domain binds as tight as a `Cn`, i.e. no codomain follows.
#define C_CN        \
              K_Cn: \
    case Tag::K_cn: \
    case Tag::K_con

/// Binders that receive an implicit `ret` continuation.
#define C_FN        \
              K_Fn: \
    case Tag::K_fn: \
    case Tag::K_fun

#define C_SEQ            \
              D_angle_l: \
    case Tag::D_quote_l

#define C_PI             \
              D_brace_l: \
    case Tag::K_Cn:      \
    case Tag::K_Fn

#define C_LM        \
              T_lm: \
    case Tag::K_cn: \
    case Tag::K_fn

#define C_EXPR                          \
              C_PRIMARY:                \
    case Tag::C_ID:                     \
    case Tag::C_LIT:                    \
    case Tag::C_DECL:                   \
    case Tag::C_PI:                     \
    case Tag::C_LM:                     \
    case Tag::K_Type:    /*TypeExpr*/   \
    case Tag::K_Rule:    /*RuleExpr*/   \
    case Tag::K_ins:     /*InsertExpr*/ \
    case Tag::K_match:   /*MatchExpr*/  \
    case Tag::K_ret:     /*RetExpr*/    \
    case Tag::C_SEQ:     /*SeqExpr*/    \
    case Tag::D_brckt_l: /*SigmaExpr*/  \
    case Tag::D_curly_l: /*UniqExpr*/   \
    case Tag::D_paren_l  /*TupleExpr*/

#define C_CURRIED_B       \
              D_brace_l:  \
    case Tag::D_brckt_l:  \
    case Tag::D_quote_l

#define C_CURRIED_P       \
              D_brace_l:  \
    case Tag::D_brckt_l:  \
    case Tag::D_paren_l
// clang-format on

/// Turns such a family into a predicate - a `case` label is of no use outside of a `switch`.
#define ISA(tag, family)                        \
    ([&] {                                      \
        switch (tag) {                          \
            case Tok::Tag::family: return true; \
            default: return false;              \
        }                                       \
    }())
