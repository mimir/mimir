#include <ostream>

#include "mim/ast/ast.h"
#include "mim/util/print.h"

namespace mim::ast {

using Tag = Tok::Tag;

struct S {
    S(Tab& tab, const Node* node)
        : tab(tab)
        , node(node) {}

    Tab& tab;
    const Node* node;

    friend std::ostream& operator<<(std::ostream& os, const S& s) { return s.node->stream(s.tab, os); }
};

template<class T>
struct R {
    R(Tab& tab, const Ptrs<T>& range)
        : tab(tab)
        , range(range)
        , f([&tab](std::ostream& os, const Ptr<T>& ptr) { ptr->stream(tab, os); }) {}

    Tab& tab;
    const Ptrs<T>& range;
    std::function<void(std::ostream&, const Ptr<T>&)> f;
};

void Node::dump() const {
    Tab tab;
    stream(tab, std::cout) << std::endl;
}

/*
 * Module
 */

std::ostream& Import::stream(Tab& tab, std::ostream& os) const { return tab.println(os, "{} '{}';", tag(), "TODO"); }

std::ostream& Module::stream(Tab& tab, std::ostream& os) const {
    for (const auto& import : imports())
        import->stream(tab, os);
    for (const auto& decl : decls())
        tab.println(os, "{}", S(tab, decl.get()));
    return os;
}

/*
 * Ptrn
 */

std::ostream& ErrorPtrn::stream(Tab&, std::ostream& os) const { return os << "<error pattern>"; }
std::ostream& AliasPtrn::stream(Tab& tab, std::ostream& os) const { return print(os, "{}: {}", S(tab, ptrn()), dbg()); }
std::ostream& GrpPtrn::stream(Tab&, std::ostream& os) const { return os << dbg(); }

std::ostream& IdPtrn::stream(Tab& tab, std::ostream& os) const {
    // clang-format off
    if ( dbg() &&  type()) return print(os, "{}: {}", dbg(), S(tab, type()));
    if ( dbg() && !type()) return print(os, "{}", dbg());
    if (!dbg() &&  type()) return print(os, "{}", S(tab, type()));
    // clang-format on
    return os << "<invalid identifier pattern>";
}

std::ostream& TuplePtrn::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{}{, }{}", delim_l(), R(tab, ptrns()), delim_r());
}

/*
 * Expr
 */

std::ostream& IdExpr::stream(Tab&, std::ostream& os) const { return print(os, "{}", dbg()); }
std::ostream& ErrorExpr::stream(Tab&, std::ostream& os) const { return os << "<error expression>"; }
std::ostream& HoleExpr::stream(Tab&, std::ostream& os) const { return os << "?"; }
std::ostream& PrimaryExpr::stream(Tab&, std::ostream& os) const { return print(os, "{}", tag()); }

std::ostream& LitExpr::stream(Tab& tab, std::ostream& os) const {
    switch (tag()) {
        case Tag::L_i: return print(os, "{}", tok().lit_i());
        case Tag::L_f: return os << std::bit_cast<double>(tok().lit_u());
        case Tag::L_s:
        case Tag::L_u:
            os << tok().lit_u();
            if (type()) print(os, ": {}", S(tab, type()));
            break;
        default: os << "TODO";
    }
    return os;
}

std::ostream& DeclExpr::stream(Tab& tab, std::ostream& os) const {
    if (is_where()) {
        tab.println(os, "{} where", S(tab, expr()));
        ++tab;
        for (const auto& decl : decls())
            tab.println(os, "{}", S(tab, decl.get()));
        --tab;
        return os;
    } else {
        for (const auto& decl : decls())
            tab.println(os, "{}", S(tab, decl.get()));
        return print(os, "{}", S(tab, expr()));
    }
}

std::ostream& TypeExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "(Type {})", S(tab, level())); }
std::ostream& RuleExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "(Rule {})", S(tab, meta_type())); }

std::ostream& ArrowExpr::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{} -> {}", S(tab, dom()), S(tab, codom()));
}

std::ostream& UnionExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "({∪ })", R(tab, types())); }

std::ostream& InjExpr::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{} inj {}", S(tab, value()), S(tab, type()));
}

std::ostream& MatchExpr::Arm::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{} => {}", S(tab, ptrn()), S(tab, body()));
}

std::ostream& MatchExpr::stream(Tab& tab, std::ostream& os) const {
    tab.println(os, "match {} with", S(tab, scrutinee()));
    ++tab;
    for (const auto& arm : arms())
        tab.println(os, "| {}", S(tab, arm.get()));
    --tab;
    return tab.println(os, "}}");
}

std::ostream& PiExpr::Dom::stream(Tab& tab, std::ostream& os) const {
    print(os, "{}{}", is_implicit() ? "." : "", S(tab, ptrn()));
    if (ret()) print(os, " -> {}", S(tab, ret()->type()));
    return os;
}

std::ostream& PiExpr::stream(Tab& tab, std::ostream& os) const {
    if (tag() != Tag::Nil) print(os, "{} ", tag());
    print(os, "{}", S(tab, dom()));
    if (codom()) print(os, " -> {}", S(tab, codom()));
    return os;
}

std::ostream& LamExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "{};", S(tab, lam())); }

std::ostream& AppExpr::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{} {} {}", S(tab, callee()), is_explicit() ? "@" : "", S(tab, arg()));
}

std::ostream& RetExpr::stream(Tab& tab, std::ostream& os) const {
    println(os, "ret {} = {} $ {};", S(tab, ptrn()), S(tab, callee()), S(tab, arg()));
    return tab.print(os, "{}", S(tab, body()));
}

std::ostream& SigmaExpr::stream(Tab& tab, std::ostream& os) const { return ptrn()->stream(tab, os); }
std::ostream& TupleExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "({, })", R(tab, elems())); }

std::ostream& SeqExpr::stream(Tab& tab, std::ostream& os) const {
    return print(os, "{}{}; {}{}", is_pack() ? "‹" : "«", S(tab, arity()), S(tab, body()), is_pack() ? "›" : "»");
}

std::ostream& ExtractExpr::stream(Tab& tab, std::ostream& os) const {
    if (auto expr = std::get_if<Ptr<Expr>>(&index())) return print(os, "{}#{}", S(tab, tuple()), S(tab, expr->get()));
    return print(os, "{}#{}", S(tab, tuple()), std::get<Dbg>(index()));
}

std::ostream& InsertExpr::stream(Tab& tab, std::ostream& os) const {
    return print(os, "ins({}, {}, {})", S(tab, tuple()), S(tab, index()), S(tab, value()));
}

std::ostream& UniqExpr::stream(Tab& tab, std::ostream& os) const { return print(os, "⦃{}⦄", S(tab, inhabitant())); }

/*
 * Decl
 */

std::ostream& AxmDecl::Alias::stream(Tab&, std::ostream& os) const { return os << dbg(); }

std::ostream& AxmDecl::stream(Tab& tab, std::ostream& os) const {
    print(os, "axm {}", dbg());
    if (num_subs() != 0) {
        os << '(';
        for (auto sep = ""; const auto& aliases : subs()) {
            print(os, "{}{ = }", sep, R(tab, aliases));
            sep = ", ";
        }
        os << ')';
    }
    print(os, ": {}", S(tab, type()));
    if (normalizer()) print(os, ", {}", normalizer());
    if (curry()) print(os, ", {}", curry());
    if (trip()) print(os, ", {}", trip());
    return os << ";";
}

std::ostream& LetDecl::stream(Tab& tab, std::ostream& os) const {
    return print(os, "let {} = {};", S(tab, ptrn()), S(tab, value()));
}

std::ostream& RecDecl::stream(Tab& tab, std::ostream& os) const {
    print(os, ".rec {}", dbg());
    if (!type()->isa<HoleExpr>()) print(os, ": {}", S(tab, type()));
    return print(os, " = {};", S(tab, body()));
}

std::ostream& LamDecl::Dom::stream(Tab& tab, std::ostream& os) const {
    print(os, "{}{}", is_implicit() ? "." : "", S(tab, ptrn()));
    if (filter()) print(os, "@({})", S(tab, filter()));
    if (ret()) print(os, ": {}", S(tab, ret()->type()));
    return os;
}

std::ostream& LamDecl::stream(Tab& tab, std::ostream& os) const {
    print(os, "{} {}", tag(), dbg());
    if (!doms().front()->ptrn()->isa<TuplePtrn>()) os << ' ';
    print(os, "{}", R(tab, doms()));
    if (codom()) print(os, ": {}", S(tab, codom()));
    if (body()) {
        if (body()->isa<DeclExpr>()) {
            os << " =" << std::endl;
            (++tab).print(os, "{}", S(tab, body()));
            --tab;
        } else {
            print(os, " = {}", S(tab, body()));
        }
    }
    return os << ';';
}

std::ostream& CDecl::stream(Tab& tab, std::ostream& os) const {
    print(os, "{} {} {}", dbg(), tag(), S(tab, dom()), S(tab, codom()));
    if (tag() == Tag::K_cfun) print(os, ": {}", S(tab, codom()));
    return os;
}

std::ostream& RuleDecl::stream(Tab& tab, std::ostream& os) const {
    return print(os, "rule {} : {} => {} when {}", S(tab, var()), S(tab, lhs()), S(tab, rhs()), S(tab, guard()));
}
} // namespace mim::ast
