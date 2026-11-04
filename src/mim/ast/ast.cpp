#include "mim/ast/ast.h"

#include <set>

#include "mim/ast/parser.h"

using namespace std::literals;

namespace mim::ast {

// Node map: Parser::import holds the slot across the nested parses that may insert further entries.
struct AST::Files : std::unordered_map<const fe::Src*, Ptr<File>> {};

AST::AST(World& world)
    : world_(&world)
    , files_(std::make_unique<Files>()) {}

AST::AST(AST&& other)
    : AST(other.world()) {
    swap(*this, other);
}

AST::~AST() = default;

std::pair<Ptr<File>&, bool> AST::file(const fe::Src* src) {
    auto [i, fresh] = files_->try_emplace(src);
    return {i->second, fresh};
}

namespace {

constexpr auto Cpp_Keywords = std::to_array<std::string_view>({
    "alignas",     "alignof",   "and",       "and_eq",    "asm",      "auto",         "bitand",
    "bitor",       "bool",      "break",     "case",      "catch",    "char",         "char8_t",
    "char16_t",    "char32_t",  "class",     "compl",     "concept",  "const",        "const_cast",
    "consteval",   "constexpr", "constinit", "continue",  "co_await", "co_return",    "co_yield",
    "decltype",    "default",   "delete",    "do",        "double",   "dynamic_cast", "else",
    "enum",        "explicit",  "export",    "extern",    "false",    "float",        "for",
    "friend",      "goto",      "if",        "inline",    "int",      "long",         "mutable",
    "namespace",   "new",       "noexcept",  "not",       "not_eq",   "nullptr",      "operator",
    "or",          "or_eq",     "private",   "protected", "public",   "register",     "reinterpret_cast",
    "requires",    "return",    "short",     "signed",    "sizeof",   "static",       "static_assert",
    "static_cast", "struct",    "switch",    "template",  "this",     "thread_local", "throw",
    "true",        "try",       "typedef",   "typeid",    "typename", "union",        "unsigned",
    "using",       "virtual",   "void",      "volatile",  "wchar_t",  "while",        "xor",
    "xor_eq",
});

constexpr auto Py_Keywords = std::to_array<std::string_view>({
    "False", "None",     "True",  "and",    "as",   "assert", "async",  "await",    "break",
    "class", "continue", "def",   "del",    "elif", "else",   "except", "finally",  "for",
    "from",  "global",   "if",    "import", "in",   "is",     "lambda", "nonlocal", "not",
    "or",    "pass",     "raise", "return", "try",  "while",  "with",   "yield",
});

/// @p sym as an identifier of the target language: a keyword gets a trailing `_`.
std::string escape(Sym sym, std::span<const std::string_view> keywords) {
    auto id = std::string(sym.view());
    return std::ranges::contains(keywords, sym.view()) ? id + "_" : id;
}

std::string_view kind(const AnnexInfo& annex) { return annex.fields ? "field" : "ctor"; }

} // namespace

void AST::bootstrap(Sym plugin, std::ostream& h) {
    auto tab = fe::Tab::spaces();
    std::println(h, "{}#pragma once\n", tab);
    std::println(h, "{}#include <mim/axm.h>", tab);
    std::println(h, "{}#include <mim/plugin.h>\n", tab);

    const auto& foreign = plugin2foreign_aliases(plugin);
    auto targets        = std::set<std::string>();
    for (const auto& alias : foreign)
        targets.emplace(alias.annex->sym.plugin.str());
    for (const auto& target : targets)
        std::println(h, "{}#include <mim/plug/{}/autogen.h>", tab, target);
    if (!targets.empty()) std::println(h, "");

    std::println(h, "{}/// @namespace mim::plug::{} @ref {}", tab, plugin, plugin);
    std::println(h, "{}namespace mim {{", tab);
    std::println(h, "{}namespace plug::{} {{\n", tab, plugin);

    plugin_t plugin_id = *Annex::mangle(plugin);
    std::vector<std::ostringstream> normalizers, outer_namespace;

    std::println(h, "{}static constexpr plugin_t Plugin_Id = 0x{:x};\n", tab, plugin_id);

    for (const auto& [alias, annex, sub] : foreign) {
        auto id  = escape(alias, Cpp_Keywords);
        auto tag = escape(annex->sym.tag, Cpp_Keywords);
        if (annex->subs.empty())
            std::println(h, "{}using {} = plug::{}::{};", tab, id, annex->sym.plugin, tag);
        else
            std::println(h, "{}static constexpr auto {} = plug::{}::{}::{};", tab, id, annex->sym.plugin, tag,
                         escape(annex->subs[sub].front(), Cpp_Keywords));
    }
    if (!foreign.empty()) std::println(h, "");

    const auto& unordered = plugin2annexes(plugin);
    std::deque<std::pair<Sym, AnnexInfo>> infos(unordered.begin(), unordered.end());
    std::ranges::sort(infos, [&](const auto& p1, const auto& p2) { return p1.second.id.tag < p2.second.id.tag; });

    // clang-format off
    for (const auto& [key, annex] : infos) {
        const auto& sym = annex.sym;
        if (sym.plugin != plugin) continue; // this is from an import
        auto tag = escape(sym.tag, Cpp_Keywords);

        std::println(h, "{}/// @name `{}.{}`\n///@{{", tab, plugin, sym.tag);
        std::println(h, "{}enum class {} : flags_t {{", tab, tag);
        ++tab;
        flags_t ax_id = annex.base();

        auto& os = outer_namespace.emplace_back();
        std::print(os, "template<> constexpr flags_t Annex::Base<plug::{}::{}> = 0x{:x};\n", plugin, tag, ax_id);

        if (auto& subs = annex.subs; !subs.empty()) {
            for (const auto& aliases : subs) {
                auto id = ax_id++;
                for (const auto alias : aliases)
                    std::println(h, "{}{} = 0x{:x},", tab, escape(alias, Cpp_Keywords), id);

                if (auto norm = annex.normalizer) {
                    auto sub = escape(aliases.front(), Cpp_Keywords);
                    auto& os = normalizers.emplace_back();
                    std::print(os, "normalizers[flags_t({}::{})] = &{}<{}::{}>;", tag, sub, norm, tag, sub);
                }
            }
        } else {
            if (auto norm = annex.normalizer)
                std::print(normalizers.emplace_back(), "normalizers[flags_t(Annex::Base<{}>)] = &{};", tag, norm);
        }
        --tab;
        std::println(h, "{}}};\n", tab);

        std::println(outer_namespace.emplace_back(), "template<> constexpr size_t Annex::Num<plug::{}::{}> = {};", plugin, tag, annex.subs.size());

        if (auto norm = annex.normalizer) {
            if (auto& subs = annex.subs; !subs.empty()) {
                std::println(h, "{}template<{}>\nconst Def* {}(const Def*, const Def*, const Def*);\n", tab, tag,
                           norm);
            } else {
                std::println(h, "{}const Def* {}(const Def*, const Def*, const Def*);", tab, norm);
            }
        }

        if (!annex.names.empty()) {
            std::println(h, "{}/// The {}s of `{}.{}`, by index.", tab, kind(annex), plugin, sym.tag);
            std::print(h, "{}struct {}_{} {{ enum : nat_t {{", tab, sym.tag, kind(annex));
            for (size_t i = 0, e = annex.names.size(); i != e; ++i)
                if (auto name = annex.names[i]) std::print(h, " {} = {},", escape(name, Cpp_Keywords), i);
            std::println(h, " }}; }};\n");
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
                     escape(sym.tag, Cpp_Keywords));
    }

    std::println(h, "{}\n#endif", tab);
    std::println(h, "{}}} // namespace mim\n", tab);

    std::println(h, "{}#ifndef DOXYGEN // don't include in Doxygen documentation\n", tab);
    for (const auto& [key, annex] : infos) {
        if (!annex.subs.empty()) {
            auto sym = annex.sym;
            std::println(h, "{}template<> struct fe::is_bit_enum<mim::plug::{}::{}> : std::true_type {{}};", tab,
                         sym.plugin, escape(sym.tag, Cpp_Keywords));
        }
    }

    std::println(h, "{}\n#endif", tab);
}

void AST::bootstrap_py(Sym plugin, std::ostream& h) {
    fe::Tab tab;
    plugin_t plugin_id = *Annex::mangle(plugin);

    std::print(h, "from enum import IntEnum\n\n");
    std::println(h, "class {}(IntEnum):", plugin);
    ++tab;
    std::println(h, "{}ID = 0x{:x}", tab, plugin_id);
    std::vector<const AnnexInfo*> annexes_with_subs;

    const auto& unordered = plugin2annexes(plugin);
    std::deque<std::pair<Sym, AnnexInfo>> infos(unordered.begin(), unordered.end());
    std::ranges::sort(infos, [&](const auto& p1, const auto& p2) { return p1.second.id.tag < p2.second.id.tag; });
    for (const auto& [key, annex] : infos) {
        const auto& sym = annex.sym;
        if (sym.plugin != plugin) continue;

        flags_t ax_id = annex.base();

        if (auto& subs = annex.subs; subs.empty())
            std::println(h, "{}{} = 0x{:x}", tab, escape(sym.tag, Py_Keywords), ax_id);
        else
            annexes_with_subs.emplace_back(&annex);
    }
    for (const auto& [alias, annex, sub] : plugin2foreign_aliases(plugin))
        std::println(h, "{}{} = 0x{:x}", tab, escape(alias, Py_Keywords), annex->base() + sub);
    std::print(h, "\n");
    --tab;

    for (auto annex : annexes_with_subs) {
        flags_t ax_id = annex->base();
        auto tag      = escape(annex->sym.tag, Py_Keywords);
        std::println(h, "class _{}_{}(IntEnum):", plugin, tag);
        ++tab;

        for (const auto& aliases : annex->subs) {
            auto id = ax_id++;
            for (const auto alias : aliases)
                std::println(h, "{}{} = 0x{:x}", tab, escape(alias, Py_Keywords), id);
        }

        --tab;
        std::println(h, "\n{}.{} = _{}_{}\n", plugin, tag, plugin, tag);
    }

    for (const auto& [key, annex] : infos) {
        if (annex.sym.plugin != plugin || annex.names.empty()) continue;
        auto type = std::format("{}_{}", annex.sym.tag, kind(annex));
        std::println(h, "class _{}_{}(IntEnum):", plugin, type);
        ++tab;
        for (size_t i = 0, e = annex.names.size(); i != e; ++i)
            if (auto name = annex.names[i]) std::println(h, "{}{} = {}", tab, escape(name, Py_Keywords), i);
        --tab;
        std::println(h, "\n{}.{} = _{}_{}\n", plugin, type, plugin, type);
    }
}

/*
 * Other
 */

Sym TuplePtrn::field(size_t i) const {
    auto p = ptrn(i);
    return (p->isa<IdPtrn>() || p->isa<GrpPtrn>()) && !p->dbg().is_anon() ? p->dbg().sym() : Sym();
}

LamExpr::LamExpr(Ptr<LamDecl> lam)
    : Expr(lam->loc())
    , lam_(lam) {}

/*
 * Ptrn::to_expr
 */

Ptr<Expr> Ptrn::to_expr(AST& ast, Ptr<Ptrn> ptrn) {
    if (auto idp = ptrn->isa<IdPtrn>(); idp && !idp->dbg() && idp->type()) {
        if (auto pe = idp->type()->isa<PathExpr>())
            return ast.ptr<PathExpr>(ast.ptr<Path>(pe->path()->loc(), pe->path()->dbgs()));
    } else if (auto tuple = ptrn->isa<TuplePtrn>(); tuple && tuple->is_brckt()) {
        return ast.ptr<SigmaExpr>(Ptr<TuplePtrn>(tuple));
    }
    return {};
}

void File::compile(AST& ast) const {
    bind(ast);
    ast.error().ack();
    emit(ast);
    ast.error().report();
}

AST load_plugins(World& world, fe::View<std::string> plugins) {
    auto tag    = world.driver().flags().bootstrap ? Tok::Tag::K_import : Tok::Tag::K_plugin;
    auto ast    = AST(world);
    auto parser = Parser(ast);

    if (!plugins.empty()) {
        auto imports = parser.import_plugins(plugins, tag);
        auto decls   = Ptrs<ValDecl>();
        for (auto import : imports)
            decls.emplace_back(import);

        // No Loc: this File spans no source, and hulling the imports would mix Loc%s of different files.
        auto file = ast.ptr<File>(Loc(), ast.scope(), ast.copy(decls));
        file->compile(ast);
    }

    return ast;
}

} // namespace mim::ast
