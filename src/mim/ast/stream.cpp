#include <bit>
#include <ostream>

#include "mim/ast/ast.h"
#include "mim/ast/lexer.h"

#include "family.h"

namespace mim::ast {

using Tag = Tok::Tag;

enum class Side { None, L, R };

/// Does an Expr at @p prec need parentheses in a slot the parser reads at @p ctx, on @p side of an operator there?
constexpr bool needs_parens(Prec prec, Prec ctx, Side side) {
    if (prec != ctx) return prec < ctx;
    switch (prec_assoc(ctx)) {
        case Assoc::L: return side == Side::R;
        case Assoc::R: return side == Side::L;
        case Assoc::N: return side != Side::None;
    }
    fe::unreachable();
}

struct S {
    S(fe::Tab& tab, const Node* node)
        : tab(tab)
        , node(node) {}
    S(fe::Tab& tab, const Expr* expr, Prec ctx, Side side = Side::None)
        : tab(tab)
        , node(expr)
        , parens(needs_parens(expr->prec(), ctx, side)) {}

    fe::Tab& tab;
    const Node* node;
    bool parens = false;

    friend std::ostream& operator<<(std::ostream& os, const S& s) {
        if (s.parens) os << '(';
        s.node->stream(s.tab, os);
        if (s.parens) os << ')';
        return os;
    }
};

} // namespace mim::ast

#ifndef DOXYGEN
template<>
struct std::formatter<mim::ast::S> : fe::ostream_formatter {};
#endif

namespace mim::ast {

template<class T>
struct R {
    R(fe::Tab& tab, fe::View<Ptr<T>> range, std::string_view sep = ", ")
        : tab(tab)
        , range(range)
        , sep(sep) {}

    fe::Tab& tab;
    fe::View<Ptr<T>> range;
    std::string_view sep;

    friend std::ostream& operator<<(std::ostream& os, const R& r) {
        for (std::string_view curr_sep{}; const auto& ptr : r.range) {
            os << curr_sep;
            ptr->stream(r.tab, os);
            curr_sep = r.sep;
        }
        return os;
    }
};

} // namespace mim::ast

#ifndef DOXYGEN
template<class T>
struct std::formatter<mim::ast::R<T>> : fe::ostream_formatter {};
#endif

namespace mim::ast {

static void stream_decls(fe::Tab& tab, std::ostream& os, fe::View<Ptr<ValDecl>> decls);
static bool is_block(const ValDecl*);

void Node::dump() const {
    auto tab = fe::Tab::spaces();
    stream(tab, std::cout);
    std::cout << std::endl;
}

/*
 * File
 */

// The output parses back into the same AST: a paren of the input is a TupleExpr, S adds the ones a slot needs.
void File::stream(fe::Tab& tab, std::ostream& os) const { stream_decls(tab, os, decls()); }

/*
 * Ptrn
 */

void ErrorPtrn::stream(fe::Tab&, std::ostream& os, Prec) const { os << "<error pattern>"; }
void GrpPtrn::stream(fe::Tab&, std::ostream& os, Prec) const { os << dbg(); }

void AliasPtrn::stream(fe::Tab& tab, std::ostream& os, Prec prec) const {
    ptrn()->stream(tab, os, prec);
    std::print(os, " as {}", dbg());
}

void IdPtrn::stream(fe::Tab& tab, std::ostream& os, Prec prec) const {
    // clang-format off
    if ( dbg() &&  type()) { std::print(os, "{}: {}", dbg(), S(tab, type(), prec)); return; }
    if ( dbg() && !type()) { std::print(os, "{}", dbg()); return; }
    if (!dbg() &&  type()) { std::print(os, "{}", S(tab, type(), prec)); return; }
    // clang-format on
    os << "<invalid identifier pattern>";
}

void TuplePtrn::stream(fe::Tab& tab, std::ostream& os, Prec) const {
    os << delim_l();
    for (std::string_view sep{}; auto ptrn : ptrns()) {
        std::print(os, "{}{}", sep, S(tab, ptrn.get()));
        sep = ptrn->isa<GrpPtrn>() ? " " : ", ";
    }
    os << delim_r();
}

/*
 * Expr
 */

void Path::stream(fe::Tab&, std::ostream& os) const { std::print(os, "{}", fe::Join(dbgs(), ".")); }

void PathExpr::stream(fe::Tab& tab, std::ostream& os) const { path()->stream(tab, os); }
void ErrorExpr::stream(fe::Tab&, std::ostream& os) const { os << "<error expression>"; }
void HoleExpr::stream(fe::Tab&, std::ostream& os) const { os << "?"; }
void RawExpr::stream(fe::Tab&, std::ostream& os) const { os << text().view(); }
void PrimaryExpr::stream(fe::Tab&, std::ostream& os) const { std::print(os, "{}", tag()); }

static std::string escape_char(char8_t c) { return c == '\'' ? "\\'" : Lexer::escape(std::string(1, char(c))); }

void LitExpr::stream(fe::Tab& tab, std::ostream& os) const {
    switch (tag()) {
        case Tag::L_i: {
            auto [size, val] = tok().lit_i();
            if (size == 0 || (size > 1 && std::has_single_bit(size)))
                std::print(os, "{}I{}", val, Idx::size2bitwidth(size));
            else
                std::print(os, "{}_{}", val, size);
            break;
        }
        case Tag::L_s: os << std::bit_cast<s64>(tok().lit_u()); break;
        case Tag::L_u: os << tok().lit_u(); break;
        case Tag::L_f: {
            auto str = std::format("{}", std::bit_cast<f64>(tok().lit_u()));
            // The shortest round-tripping spelling may lack both `.` and exponent, which would lex as an integer.
            if (str.find_first_of(".ein") == std::string::npos) str += ".0";
            os << str;
            break;
        }
        case Tag::L_c: std::print(os, "'{}'", escape_char(tok().lit_c())); break;
        case Tag::L_str: std::print(os, "\"{}\"", Lexer::escape(tok().sym().view())); break;
        case Tag::T_bot:
        case Tag::T_top: os << tag(); break;
        default: fe::unreachable();
    }
    if (type()) std::print(os, ":{}", S(tab, type(), Prec::Lit));
}

void DeclExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (is_where()) {
        std::println(os, "{} where", S(tab, expr()));
        ++tab;
        stream_decls(tab, os, decls());
        --tab;
        std::print(os, "{}end", tab);
    } else {
        bool prev = false;
        for (size_t i = 0; auto decl : decls()) {
            auto block = is_block(decl.get());
            if (i++ != 0) std::print(os, "\n{}{}", prev || block ? "\n" : "", tab);
            std::print(os, "{}", S(tab, decl.get()));
            prev = block;
        }
        if (!decls().empty()) std::print(os, "\n{}{}", prev ? "\n" : "", tab);
        std::print(os, "{}", S(tab, expr()));
    }
}

void TypeExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "Type {}", S(tab, level(), Prec::App, Side::R));
}

void RuleExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "Rule {}", S(tab, dom(), Prec::App, Side::R));
}

void PrefixExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}", Tok::tag2str(op().tag()), S(tab, rhs(), Prec::Prefix));
}

void InfixExpr::stream(fe::Tab& tab, std::ostream& os) const {
    auto tag  = op().tag();
    auto prec = *Tok::infix_prec(tag);
    auto l    = S(tab, lhs(), prec, Side::L);
    auto r    = S(tab, rhs(), prec, Side::R);
    if (tag == Tag::T_extract) return std::print(os, "{}{}{}", l, Tok::tag2str(tag), r);
    std::print(os, "{} {} {}", l, Tok::tag2str(tag), r);
}

// An arm's body and a constructor's type print at Where: the parser reads them at Bot, but a Bot child would swallow
// the `|` that follows.
void MatchExpr::Arm::stream(fe::Tab& tab, std::ostream& os) const {
    auto sel = index() ? std::format("{}", *index()) : std::format("{}", S(tab, ptrn()));
    if (payload())
        std::print(os, "{} {}", sel, S(tab, payload()));
    else
        std::print(os, "{}", sel);
    std::print(os, " => {}", S(tab, body(), Prec::Where));
}

void VariantExpr::Ctor::stream(fe::Tab& tab, std::ostream& os) const {
    if (type()) return std::print(os, "{}: {}", dbg(), S(tab, type(), Prec::Where));
    std::print(os, "{}", dbg());
}

void VariantExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (num_ctors() == 0) return std::print(os, "|");
    for (auto sep = ""; auto ctor : ctors()) {
        std::print(os, "{}| {}", sep, S(tab, ctor.get()));
        sep = " ";
    }
}

void MatchExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "match {} with", S(tab, scrutinee()));
    ++tab;
    for (auto arm : arms())
        std::print(os, "\n{}| {}", tab, S(tab, arm.get()));
    --tab;
}

void PiExpr::Dom::stream(fe::Tab& tab, std::ostream& os, Prec prec) const {
    ptrn()->stream(tab, os, prec);
    if (ret()) std::print(os, " {} {}", Tag::T_arrow_r, S(tab, ret()->type(), Prec::Arrow, Side::R));
}

void PiExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (tag() == Tag::K_Cn || tag() == Tag::K_Fn) std::print(os, "{} ", tag());
    dom()->stream(tab, os, tag() == Tag::K_Cn ? Prec::Bot : Prec::Pi);
    if (codom()) std::print(os, " {} {}", Tag::T_arrow_r, S(tab, codom(), Prec::Arrow, Side::R));
}

void LamExpr::stream(fe::Tab& tab, std::ostream& os) const { lam()->stream_chain(tab, os); }

void AppExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} {}", S(tab, callee(), Prec::App, Side::L), S(tab, arg(), Prec::App, Side::R));
}

void RetExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::println(os, "ret {} = {} $ {};", S(tab, ptrn()), S(tab, callee()), S(tab, arg()));
    std::print(os, "{}{}", tab, S(tab, body()));
}

void SigmaExpr::stream(fe::Tab& tab, std::ostream& os) const { ptrn()->stream(tab, os); }
void TupleExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "({})", R(tab, elems())); }

void SeqExpr::stream(fe::Tab& tab, std::ostream& os) const {
    os << (is_pack() ? "‹" : "«");
    // `«a, b; e»` is sugar for one SeqExpr per arity, so a directly nested one reads as a further axis.
    auto curr = this;
    for (auto sep = "";; sep = ", ") {
        std::print(os, "{}{}", sep, S(tab, curr->arity()));
        auto inner = curr->body()->isa<SeqExpr>();
        if (!inner || inner->is_pack() != is_pack()) break;
        curr = inner;
    }
    std::print(os, "; {}{}", S(tab, curr->body()), is_pack() ? "›" : "»");
}

void SingleExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}{}", is_wrap() ? "‹" : "«", S(tab, body()), is_wrap() ? "›" : "»");
}

/*
 * Decl
 */

static std::string_view vis2str(Vis vis) {
    switch (vis) {
        case Vis::Priv: return "priv";
        case Vis::Pub: return "pub";
    }
    fe::unreachable();
}

/// Prints `vis`/`extern`/`anx`, skipping `vis` if it's the modifier-nudged Mods::default_vis.
static std::ostream& operator<<(std::ostream& os, const Mods& mods) {
    if (auto vis = mods.resolved_vis(); vis != mods.default_vis()) std::print(os, "{} ", vis2str(vis));
    if (mods.is_extern) std::print(os, "extern ");
    if (mods.is_anx) std::print(os, "anx ");
    return os;
}

} // namespace mim::ast

#ifndef DOXYGEN
template<>
struct std::formatter<mim::ast::Mods> : fe::ostream_formatter {};
#endif

namespace mim::ast {

/// The members of an `axm` group in @p decls, starting at @p i with its owning AxmDecl; see Parser::parse_axm_group.
static size_t axm_group_end(fe::View<Ptr<ValDecl>> decls, size_t i) {
    auto owner    = decls[i]->as<AxmDecl>();
    auto prev     = owner->dbg().sym();
    auto is_alias = [&](const ValDecl* decl) {
        auto alias = decl->isa<AliasDecl>();
        return alias && alias->vis() == Vis::Pub && alias->path()->dbgs().size() == 1
            && alias->path()->front().sym() == prev;
    };
    for (++i; i != decls.size(); ++i)
        if (auto sibling = decls[i]->isa<AxmDecl::Sibling>(); sibling && sibling->owner() == owner)
            prev = sibling->dbg().sym();
        else if (!is_alias(decls[i].get()))
            break;
    return i;
}

static void stream_axm_tail(fe::Tab& tab, std::ostream& os, const AxmDecl* axm) {
    std::print(os, ": {}", S(tab, axm->type()));
    if (axm->normalizer()) std::print(os, ", {}", axm->normalizer());
    if (axm->curry()) std::print(os, ", {}", axm->curry().lit_u());
    if (axm->trip()) std::print(os, ", {}", axm->trip().lit_u());
    os << ';';
}

/// `axm [mod.](tag_0 = alias, ..., tag_n-1): ...;` of the `axm` group `decls[begin, end)`.
static void
stream_axm_group(fe::Tab& tab, std::ostream& os, fe::View<Ptr<ValDecl>> decls, size_t begin, size_t end, Dbg mod = {}) {
    if (decls[begin]->vis() == Vis::Priv) os << "priv ";
    os << "axm ";
    if (mod) std::print(os, "{}.", mod);
    os << '(';
    for (auto i = begin; i != end; ++i)
        if (decls[i]->isa<AliasDecl>())
            std::print(os, " = {}", decls[i]->dbg());
        else
            std::print(os, "{}{}", i == begin ? "" : ", ", decls[i]->dbg());
    os << ')';
    stream_axm_tail(tab, os, decls[begin]->as<AxmDecl>());
}

/// `axm tag.(sub_0, ..., sub_n-1): ...;` desugars to exactly such a `pub mod`, so that is how it prints again.
static bool is_axm_group(const ModDecl* mod) {
    auto decls = mod->decls();
    return !decls.empty() && decls.front()->isa<AxmDecl>() && mod->vis() == Vis::Pub
        && axm_group_end(decls, 0) == decls.size();
}

/// Spans several lines, so a blank line sets it apart from its neighbors.
static bool is_block(const ValDecl* decl) {
    if (auto mod = decl->isa<ModDecl>()) return !is_axm_group(mod);
    if (auto rec = decl->isa<RecDecl>()) return rec->next() || (rec->body() && rec->body()->isa<DeclExpr>());
    return false;
}

static void stream_decls(fe::Tab& tab, std::ostream& os, fe::View<Ptr<ValDecl>> decls) {
    bool prev = false;
    for (size_t i = 0, end; i != decls.size(); i = end) {
        // Siblings share their owner's type, so they must stay in the group that introduces them.
        end        = decls[i]->isa<AxmDecl>() ? axm_group_end(decls, i) : i + 1;
        auto block = end == i + 1 && is_block(decls[i].get());
        if (prev || (block && i != 0)) os << '\n';
        os << tab;
        if (end != i + 1)
            stream_axm_group(tab, os, decls, i, end), os << '\n';
        else
            std::println(os, "{}", S(tab, decls[i].get()));
        prev = block;
    }
}

/// `axm` is always anx, so only `priv` is ever printed.
static void stream_axm(fe::Tab& tab, std::ostream& os, const ValDecl* decl, const AxmDecl* owner) {
    if (decl->vis() == Vis::Priv) os << "priv ";
    std::print(os, "axm {}", decl->dbg());
    stream_axm_tail(tab, os, owner);
}

void AxmDecl::stream(fe::Tab& tab, std::ostream& os) const { stream_axm(tab, os, this, this); }
void AxmDecl::Sibling::stream(fe::Tab& tab, std::ostream& os) const { stream_axm(tab, os, this, owner()); }

void AliasDecl::stream(fe::Tab& tab, std::ostream& os) const {
    if (vis() == Vis::Priv) std::print(os, "priv ");
    std::print(os, "anx {} = {};", dbg(), S(tab, path()));
}

void ModDecl::stream(fe::Tab& tab, std::ostream& os) const {
    if (is_axm_group(this)) return stream_axm_group(tab, os, decls(), 0, decls().size(), dbg());

    std::println(os, "{}mod {} {{", mods(), dbg());
    ++tab;
    stream_decls(tab, os, decls());
    --tab;
    std::print(os, "{}}}", tab);
}

void UseDecl::stream(fe::Tab& tab, std::ostream& os) const {
    if (is_file_path())
        std::print(os, "{}{} \"{}\"", mods(), tag(), Lexer::escape(file_path().view()));
    else
        std::print(os, "{}{} {}", mods(), tag(), S(tab, path()));
    if (alias()) std::print(os, " as {}", alias());
    if (is_splice() && is_import()) std::print(os, " as {}", Tag::T_star);
    os << ';';
}

void LetDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}let {} = {};", mods(), S(tab, ptrn()), S(tab, value()));
}

void RecDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}", mods());
    stream_chain(tab, os);
    os << ';';
}

void RecDecl::stream_chain(fe::Tab& tab, std::ostream& os) const {
    if (!isa<LamDecl>()) os << "rec ";
    stream_(tab, os);
    for (auto curr = next(); curr; curr = curr->next()) {
        std::println(os);
        std::print(os, "{}and ", tab);
        curr->stream_(tab, os);
    }
}

void RecDecl::stream_(fe::Tab& tab, std::ostream& os) const { std::print(os, "{} = {}", dbg(), S(tab, body())); }

/// The `: codom` slot ends at `=`, so the parser reads it just above Where; see Parser::parse_lam_decl.
static constexpr auto Prec_Codom = Prec(int(Prec::Where) + 1);

void LamDecl::Dom::stream(fe::Tab& tab, std::ostream& os, Prec prec) const {
    ptrn()->stream(tab, os, prec);
    if (filter()) std::print(os, "@{}", S(tab, filter()));
    // Parser::parse_lam_decl fills in an omitted codomain of a `fun`/`fn` as a hole.
    if (ret() && !ret()->type()->isa<HoleExpr>()) std::print(os, ": {}", S(tab, ret()->type(), Prec_Codom));
}

/// Does @p expr span several statements and hence deserve an indented block of its own?
static bool is_block(const Expr* expr) {
    if (auto decl = expr->isa<DeclExpr>()) return !decl->is_where();
    return expr->isa<RetExpr>();
}

void LamDecl::stream_(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}", tag());
    if (dbg()) std::print(os, " {}", dbg());
    auto prec = ISA(tag(), C_CN) ? Prec::Bot : Prec::Pi;
    for (auto dom : doms()) {
        os << ' ';
        dom->stream(tab, os, prec);
    }
    if (codom()) std::print(os, ": {}", S(tab, codom(), Prec_Codom));
    if (body()) {
        if (is_block(body())) {
            ++tab;
            std::print(os, " =\n{}{}", tab, S(tab, body()));
            --tab;
        } else {
            std::print(os, " = {}", S(tab, body()));
        }
    }
}

void RuleDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} {} {}: {}", is_normalizer() ? Tag::K_norm : Tag::K_rule, dbg(), S(tab, var()), S(tab, lhs()));
    if (auto tt = guard()->isa<PrimaryExpr>(); !tt || tt->tag() != Tag::K_tt)
        std::print(os, " when {}", S(tab, guard()));
    std::print(os, " => {};", S(tab, rhs()));
}

} // namespace mim::ast
