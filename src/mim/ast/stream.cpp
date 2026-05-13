#include <ostream>

#include "mim/ast/ast.h"

namespace mim::ast {

using Tag = Tok::Tag;

struct S {
    S(fe::Tab& tab, const Node* node)
        : tab(tab)
        , node(node) {}

    fe::Tab& tab;
    const Node* node;

    friend std::ostream& operator<<(std::ostream& os, const S& s) { return s.node->stream(s.tab, os), os; }
};

} // namespace mim::ast

template<>
struct std::formatter<mim::ast::S> : fe::ostream_formatter {};

namespace mim::ast {

template<class T>
struct R {
    R(fe::Tab& tab, const Ptrs<T>& range, std::string_view sep = ", ")
        : tab(tab)
        , range(range)
        , sep(sep) {}

    fe::Tab& tab;
    const Ptrs<T>& range;
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

template<class T>
struct std::formatter<mim::ast::R<T>> : fe::ostream_formatter {};

namespace mim::ast {

void Node::dump() const {
    auto tab = fe::Tab::spaces();
    stream(tab, std::cout);
    std::cout << std::endl;
}

/*
 * Module
 */

void Import::stream(fe::Tab& tab, std::ostream& os) const { std::println(os, "{}{} '{}';", tab, tag(), "TODO"); }

void Module::stream(fe::Tab& tab, std::ostream& os) const {
    for (const auto& import : imports())
        import->stream(tab, os);
    for (const auto& decl : decls())
        std::println(os, "{}{}", tab, S(tab, decl.get()));
}

/*
 * Ptrn
 */

void ErrorPtrn::stream(fe::Tab&, std::ostream& os) const { os << "<error pattern>"; }
void AliasPtrn::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "{}: {}", S(tab, ptrn()), dbg()); }
void GrpPtrn::stream(fe::Tab&, std::ostream& os) const { os << dbg(); }

void IdPtrn::stream(fe::Tab& tab, std::ostream& os) const {
    // clang-format off
    if ( dbg() &&  type()) { std::print(os, "{}: {}", dbg(), S(tab, type())); return; }
    if ( dbg() && !type()) { std::print(os, "{}", dbg()); return; }
    if (!dbg() &&  type()) { std::print(os, "{}", S(tab, type())); return; }
    // clang-format on
    os << "<invalid identifier pattern>";
}

void TuplePtrn::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}{}", delim_l(), R(tab, ptrns()), delim_r());
}

/*
 * Expr
 */

void IdExpr::stream(fe::Tab&, std::ostream& os) const { std::print(os, "{}", dbg()); }
void ErrorExpr::stream(fe::Tab&, std::ostream& os) const { os << "<error expression>"; }
void HoleExpr::stream(fe::Tab&, std::ostream& os) const { os << "?"; }
void PrimaryExpr::stream(fe::Tab&, std::ostream& os) const { std::print(os, "{}", tag()); }

void LitExpr::stream(fe::Tab& tab, std::ostream& os) const {
    switch (tag()) {
        case Tag::L_i: std::print(os, "{}", tok().lit_i()); return;
        case Tag::L_f: os << std::bit_cast<double>(tok().lit_u()); return;
        case Tag::L_s:
        case Tag::L_u:
            os << tok().lit_u();
            if (type()) std::print(os, ": {}", S(tab, type()));
            return;
        default: os << "TODO";
    }
}

void DeclExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (is_where()) {
        std::println(os, "{}{} where", tab, S(tab, expr()));
        ++tab;
        for (const auto& decl : decls())
            std::println(os, "{}{}", tab, S(tab, decl.get()));
        --tab;
    } else {
        for (const auto& decl : decls())
            std::println(os, "{}{}", tab, S(tab, decl.get()));
        std::print(os, "{}", S(tab, expr()));
    }
}

void TypeExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "(Type {})", S(tab, level())); }
void RuleExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "(Rule {})", S(tab, dom())); }

void ArrowExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} -> {}", S(tab, dom()), S(tab, codom()));
}

void UnionExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "({})", R(tab, types(), "∪ ")); }

void InjExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} inj {}", S(tab, value()), S(tab, type()));
}

void MatchExpr::Arm::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} => {}", S(tab, ptrn()), S(tab, body()));
}

void MatchExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::println(os, "{}match {} with", tab, S(tab, scrutinee()));
    ++tab;
    for (const auto& arm : arms())
        std::println(os, "{}| {}", tab, S(tab, arm.get()));
    --tab;
    std::println(os, "{}}}", tab);
}

void PiExpr::Dom::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}", is_implicit() ? "." : "", S(tab, ptrn()));
    if (ret()) std::print(os, " -> {}", S(tab, ret()->type()));
}

void PiExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (tag() != Tag::Nil) std::print(os, "{} ", tag());
    std::print(os, "{}", S(tab, dom()));
    if (codom()) std::print(os, " -> {}", S(tab, codom()));
}

void LamExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "{};", S(tab, lam())); }

void AppExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} {} {}", S(tab, callee()), is_explicit() ? "@" : "", S(tab, arg()));
}

void RetExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::println(os, "ret {} = {} $ {};", S(tab, ptrn()), S(tab, callee()), S(tab, arg()));
    std::print(os, "{}{}", tab, S(tab, body()));
}

void SigmaExpr::stream(fe::Tab& tab, std::ostream& os) const { ptrn()->stream(tab, os); }
void TupleExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "({})", R(tab, elems())); }

void SeqExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}; {}{}", is_pack() ? "‹" : "«", S(tab, arity()), S(tab, body()), is_pack() ? "›" : "»");
}

void ExtractExpr::stream(fe::Tab& tab, std::ostream& os) const {
    if (auto expr = std::get_if<Ptr<Expr>>(&index()))
        std::print(os, "{}#{}", S(tab, tuple()), S(tab, expr->get()));
    else
        std::print(os, "{}#{}", S(tab, tuple()), std::get<Dbg>(index()));
}

void InsertExpr::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "ins({}, {}, {})", S(tab, tuple()), S(tab, index()), S(tab, value()));
}

void UniqExpr::stream(fe::Tab& tab, std::ostream& os) const { std::print(os, "⦃{}⦄", S(tab, inhabitant())); }

/*
 * Decl
 */

void AxmDecl::Alias::stream(fe::Tab&, std::ostream& os) const { os << dbg(); }

void AxmDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "axm {}", dbg());
    if (num_subs() != 0) {
        os << '(';
        for (auto sep = ""; const auto& aliases : subs()) {
            std::print(os, "{}{}", sep, R(tab, aliases, " = "));
            sep = ", ";
        }
        os << ')';
    }
    std::print(os, ": {}", S(tab, type()));
    if (normalizer()) std::print(os, ", {}", normalizer());
    if (curry()) std::print(os, ", {}", curry());
    if (trip()) std::print(os, ", {}", trip());
    os << ";";
}

void LetDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "let {} = {};", S(tab, ptrn()), S(tab, value()));
}

void RecDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, ".rec {}", dbg());
    if (!type()->isa<HoleExpr>()) std::print(os, ": {}", S(tab, type()));
    std::print(os, " = {};", S(tab, body()));
}

void LamDecl::Dom::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{}{}", is_implicit() ? "." : "", S(tab, ptrn()));
    if (filter()) std::print(os, "@({})", S(tab, filter()));
    if (ret()) std::print(os, ": {}", S(tab, ret()->type()));
}

void LamDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} {}", tag(), dbg());
    if (!doms().front()->ptrn()->isa<TuplePtrn>()) os << ' ';
    std::print(os, "{}", R(tab, doms()));
    if (codom()) std::print(os, ": {}", S(tab, codom()));
    if (body()) {
        if (body()->isa<DeclExpr>()) {
            os << " =" << std::endl;
            ++tab;
            std::print(os, "{}{}", tab, S(tab, body()));
            --tab;
        } else {
            std::print(os, " = {}", S(tab, body()));
        }
    }
    os << ';';
}

void CDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "{} {} {}", dbg(), tag(), S(tab, dom()), S(tab, codom()));
    if (tag() == Tag::K_cfun) std::print(os, ": {}", S(tab, codom()));
}

void RuleDecl::stream(fe::Tab& tab, std::ostream& os) const {
    std::print(os, "rule {} : {} => {} when {}", S(tab, var()), S(tab, lhs()), S(tab, rhs()), S(tab, guard()));
}
} // namespace mim::ast
