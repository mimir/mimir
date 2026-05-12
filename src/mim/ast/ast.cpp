#include "mim/ast/ast.h"

#include "mim/ast/parser.h"

using namespace std::literals;

namespace mim::ast {

namespace {
/// Returns a C++-safe identifier: prepends '_' if @p s is a C++ keyword.
constexpr std::string_view escape_keywords(std::string_view s) {
    // clang-format off
    if (s == "alignas")         return "_alignas";
    if (s == "alignof")         return "_alignof";
    if (s == "and")             return "_and";
    if (s == "and_eq")          return "_and_eq";
    if (s == "asm")             return "_asm";
    if (s == "auto")            return "_auto";
    if (s == "bitand")          return "_bitand";
    if (s == "bitor")           return "_bitor";
    if (s == "bool")            return "_bool";
    if (s == "break")           return "_break";
    if (s == "case")            return "_case";
    if (s == "catch")           return "_catch";
    if (s == "char")            return "_char";
    if (s == "char8_t")         return "_char8_t";
    if (s == "char16_t")        return "_char16_t";
    if (s == "char32_t")        return "_char32_t";
    if (s == "class")           return "_class";
    if (s == "compl")           return "_compl";
    if (s == "concept")         return "_concept";
    if (s == "const")           return "_const";
    if (s == "consteval")       return "_consteval";
    if (s == "constexpr")       return "_constexpr";
    if (s == "constinit")       return "_constinit";
    if (s == "const_cast")      return "_const_cast";
    if (s == "continue")        return "_continue";
    if (s == "co_await")        return "_co_await";
    if (s == "co_return")       return "_co_return";
    if (s == "co_yield")        return "_co_yield";
    if (s == "decltype")        return "_decltype";
    if (s == "default")         return "_default";
    if (s == "delete")          return "_delete";
    if (s == "do")              return "_do";
    if (s == "double")          return "_double";
    if (s == "dynamic_cast")    return "_dynamic_cast";
    if (s == "else")            return "_else";
    if (s == "enum")            return "_enum";
    if (s == "explicit")        return "_explicit";
    if (s == "export")          return "_export";
    if (s == "extern")          return "_extern";
    if (s == "false")           return "_false";
    if (s == "float")           return "_float";
    if (s == "for")             return "_for";
    if (s == "friend")          return "_friend";
    if (s == "goto")            return "_goto";
    if (s == "if")              return "_if";
    if (s == "inline")          return "_inline";
    if (s == "int")             return "_int";
    if (s == "long")            return "_long";
    if (s == "mutable")         return "_mutable";
    if (s == "namespace")       return "_namespace";
    if (s == "new")             return "_new";
    if (s == "noexcept")        return "_noexcept";
    if (s == "not")             return "_not";
    if (s == "not_eq")          return "_not_eq";
    if (s == "nullptr")         return "_nullptr";
    if (s == "operator")        return "_operator";
    if (s == "or")              return "_or";
    if (s == "or_eq")           return "_or_eq";
    if (s == "private")         return "_private";
    if (s == "protected")       return "_protected";
    if (s == "public")          return "_public";
    if (s == "register")        return "_register";
    if (s == "reinterpret_cast")return "_reinterpret_cast";
    if (s == "requires")        return "_requires";
    if (s == "return")          return "_return";
    if (s == "short")           return "_short";
    if (s == "signed")          return "_signed";
    if (s == "sizeof")          return "_sizeof";
    if (s == "static")          return "_static";
    if (s == "static_assert")   return "_static_assert";
    if (s == "static_cast")     return "_static_cast";
    if (s == "struct")          return "_struct";
    if (s == "switch")          return "_switch";
    if (s == "template")        return "_template";
    if (s == "this")            return "_this";
    if (s == "thread_local")    return "_thread_local";
    if (s == "throw")           return "_throw";
    if (s == "true")            return "_true";
    if (s == "try")             return "_try";
    if (s == "typedef")         return "_typedef";
    if (s == "typeid")          return "_typeid";
    if (s == "typename")        return "_typename";
    if (s == "union")           return "_union";
    if (s == "unsigned")        return "_unsigned";
    if (s == "using")           return "_using";
    if (s == "virtual")         return "_virtual";
    if (s == "void")            return "_void";
    if (s == "volatile")        return "_volatile";
    if (s == "wchar_t")         return "_wchar_t";
    if (s == "while")           return "_while";
    if (s == "xor")             return "_xor";
    if (s == "xor_eq")          return "_xor_eq";
    return s;
    // clang-format on
}
} // namespace

AST::~AST() {
    assert(error().num_errors() == 0 && error().num_warnings() == 0
           && "please encounter any errors before destroying this class");
}

Import::Import(Loc loc, Tok::Tag tag, Dbg dbg, Ptr<Module>&& module)
    : Node(loc)
    , dbg_(dbg)
    , tag_(tag)
    , module_(std::move(module)) {}

Import::~Import() = default;

AnnexInfo* AST::name2annex(Dbg dbg, sub_t* sub_id) {
    if (!dbg || dbg.sym()[0] != '%') return nullptr;

    auto [plugin_s, tag_s, sub_s] = Annex::split(driver(), dbg.sym());
    auto plugin_tag               = driver().sym("%"s + plugin_s.str() + "."s + tag_s.str());
    auto& sym2annex               = plugin2sym2annex_[plugin_s];
    auto tag_id                   = sym2annex.size();

    if (plugin_s == sym_error()) error(dbg.loc(), "plugin name '{}' is reserved", dbg);
    if (tag_id > std::numeric_limits<tag_t>::max())
        error(dbg.loc(), "exceeded maxinum number of annexes in current plugin");

    plugin_t plugin_id;
    if (auto p = Annex::mangle(plugin_s))
        plugin_id = *p;
    else {
        error(dbg.loc(), "invalid annex name '{}'", dbg);
        plugin_s  = sym_error();
        plugin_id = *Annex::mangle(plugin_s);
    }

    auto [i, fresh] = sym2annex.try_emplace(plugin_tag, AnnexInfo{plugin_s, tag_s, plugin_id, (tag_t)sym2annex.size()});
    auto annex      = &i->second;

    if (sub_s) {
        if (sub_id) {
            *sub_id       = annex->subs.size();
            auto& aliases = annex->subs.emplace_back();
            aliases.emplace_back(sub_s);
        } else {
            error(dbg.loc(), "annex '{}' must not have a subtag", dbg);
        }
    }

    if (!fresh) annex->fresh = false;
    return annex;
}

void AST::bootstrap(Sym plugin, std::ostream& h) {
    auto tab = fe::Tab::spaces();
    std::println(h, "{}#pragma once\n", tab);
    std::println(h, "{}#include <mim/axm.h>", tab);
    std::println(h, "#include <mim/plugin.h>\n", tab);
    std::println(h, "{}/// @namespace mim::plug::{} @ref {} ", tab, plugin, plugin);
    std::println(h, "{}namespace mim {{", tab);
    std::println(h, "{}namespace plug::{} {{\n", tab, plugin);

    plugin_t plugin_id = *Annex::mangle(plugin);
    std::vector<std::ostringstream> normalizers, outer_namespace;

    std::println(h, "{}static constexpr plugin_t Plugin_Id = 0x{:x};\n", tab, plugin_id);

    const auto& unordered = plugin2annexes(plugin);
    std::deque<std::pair<Sym, AnnexInfo>> infos(unordered.begin(), unordered.end());
    std::ranges::sort(infos, [&](const auto& p1, const auto& p2) { return p1.second.id.tag < p2.second.id.tag; });

    // clang-format off
    for (const auto& [key, annex] : infos) {
        const auto& sym = annex.sym;
        if (sym.plugin != plugin) continue; // this is from an import

        auto safe_tag = escape_keywords(sym.tag.view());
        std::println(h, "{}/// @name %%{}.{}\n///@{{", tab, plugin, sym.tag);
        std::println(h, "{}enum class {} : flags_t {{", tab, safe_tag);
        ++tab;
        flags_t ax_id = plugin_id | (annex.id.tag << 8u);

        auto& os = outer_namespace.emplace_back();
        std::print(os, "template<> constexpr flags_t Annex::Base<plug::{}::{}> = 0x{:x};\n", plugin, safe_tag, ax_id);

        if (auto& subs = annex.subs; !subs.empty()) {
            for (const auto& aliases : subs) {
                const auto& sub = aliases.front();
                auto safe_sub = escape_keywords(sub.view());
                std::println(h, "{}{} = 0x{:x},", tab, safe_sub, ax_id++);
                for (size_t i = 1; i < aliases.size(); ++i)
                    std::println(h, "{}{} = {},", tab, escape_keywords(aliases[i].view()), safe_sub);

                if (auto norm = annex.normalizer) {
                    auto& os = normalizers.emplace_back();
                    std::print(os, "normalizers[flags_t({}::{})] = &{}<{}::{}>;", safe_tag, safe_sub, norm, safe_tag, safe_sub);
                }
            }
        } else {
            if (auto norm = annex.normalizer)
                std::print(normalizers.emplace_back(), "normalizers[flags_t(Annex::Base<{}>)] = &{};", safe_tag, norm);
        }
        --tab;
        std::println(h, "{}}};\n", tab);

        std::println(outer_namespace.emplace_back(), "template<> constexpr size_t Annex::Num<plug::{}::{}> = {};", plugin, safe_tag, annex.subs.size());

        if (auto norm = annex.normalizer) {
            if (auto& subs = annex.subs; !subs.empty()) {
                std::println(h, "{}template<{}>\nconst Def* {}(const Def*, const Def*, const Def*);\n", tab, safe_tag,
                           norm);
            } else {
                std::println(h, "{}const Def* {}(const Def*, const Def*, const Def*);", tab, norm);
            }
        }
        std::println(h, "{}///@}}\n", tab);
    }
    // clang-format on

    if (!normalizers.empty()) {
        std::println(h, "{}void register_normalizers(Normalizers& normalizers);\n", tab);
        std::println(h, "{}#define MIM_{}_NORMALIZER_IMPL \\", tab, plugin);
        ++tab;
        std::println(h, "{}void register_normalizers(Normalizers& normalizers) {{\\", tab);
        ++tab;
        for (const auto& normalizer : normalizers)
            std::println(h, "{}{} \\", tab, normalizer.str());
        --tab;
        std::println(h, "{}}}", tab);
        --tab;
    }

    std::println(h, "{}}} // namespace plug::{}\n", tab, plugin);

    std::println(h, "{}#ifndef DOXYGEN // don't include in Doxygen documentation\n", tab);
    for (const auto& line : outer_namespace)
        std::print(h, "{}{}", tab, line.str());
    std::println(h, "{}", tab);

    // emit helpers for non-function axm
    for (const auto& [tag, ax] : infos) {
        auto sym = ax.sym;
        if ((ax.pi && *ax.pi) || sym.plugin != plugin) continue; // from function or other plugin?
        std::println(h, "{}template<> struct Axm::IsANode<plug::{}::{}> {{ using type = Axm; }};", tab, sym.plugin,
                     escape_keywords(sym.tag.view()));
    }

    std::println(h, "{}\n#endif", tab);
    std::println(h, "{}}} // namespace mim\n", tab);

    std::println(h, "{}#ifndef DOXYGEN // don't include in Doxygen documentation\n", tab);
    for (const auto& [key, annex] : infos) {
        if (!annex.subs.empty()) {
            auto sym = annex.sym;
            std::println(h, "{}template<> struct fe::is_bit_enum<mim::plug::{}::{}> : std::true_type {{}};", tab,
                         sym.plugin, escape_keywords(sym.tag.view()));
        }
    }

    std::println(h, "{}\n#endif", tab);
}

/*
 * Other
 */

LamExpr::LamExpr(Ptr<LamDecl>&& lam)
    : Expr(lam->loc())
    , lam_(std::move(lam)) {}

/*
 * Ptrn::to_expr/to_ptrn
 */

Ptr<Expr> Ptrn::to_expr(AST& ast, Ptr<Ptrn>&& ptrn) {
    if (auto idp = ptrn->isa<IdPtrn>(); idp && !idp->dbg() && idp->type()) {
        if (auto ide = idp->type()->isa<IdExpr>()) return ast.ptr<IdExpr>(ide->dbg());
    } else if (auto tuple = ptrn->isa<TuplePtrn>(); tuple && tuple->is_brckt()) {
        (void)ptrn.release();
        return ast.ptr<SigmaExpr>(Ptr<TuplePtrn>(tuple));
    }
    return {};
}

Ptr<Ptrn> Ptrn::to_ptrn(Ptr<Expr>&& expr) {
    if (auto sigma = expr->isa<SigmaExpr>())
        return std::move(const_cast<SigmaExpr*>(sigma)->ptrn_); // TODO get rid off const_cast
    return {};
}

void Module::compile(AST& ast) const {
    bind(ast);
    ast.error().ack();
    emit(ast);
    if (ast.error().num_warnings() != 0) std::cerr << ast.error();
}

AST load_plugins(World& world, View<Sym> plugins) {
    auto tag = Tok::Tag::K_import;
    if (!world.driver().flags().bootstrap) {
        for (auto plugin : plugins)
            world.driver().load(plugin);
        tag = Tok::Tag::K_plugin;
    }

    auto ast     = AST(world);
    auto parser  = Parser(ast);
    auto imports = Ptrs<Import>();

    for (auto plugin : plugins)
        if (auto mod = parser.import(plugin.view(), tag))
            imports.emplace_back(ast.ptr<Import>(mod->loc(), tag, Dbg(plugin), std::move(mod)));

    if (!plugins.empty()) {
        auto mod = ast.ptr<Module>(imports.front()->loc() + imports.back()->loc(), std::move(imports), Ptrs<ValDecl>());
        mod->compile(ast);
    }

    return ast;
}

} // namespace mim::ast
