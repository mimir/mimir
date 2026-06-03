#include "mim/ast/ast.h"

#include "mim/ast/parser.h"

using namespace std::literals;

namespace mim::ast {

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

        std::println(h, "{}/// @name %%{}.{}\n///@{{", tab, plugin, sym.tag);
        std::println(h, "{}enum class {} : flags_t {{", tab, sym.tag);
        ++tab;
        flags_t ax_id = plugin_id | (annex.id.tag << 8u);

        auto& os = outer_namespace.emplace_back();
        std::print(os, "template<> constexpr flags_t Annex::Base<plug::{}::{}> = 0x{:x};\n", plugin, sym.tag, ax_id);

        if (auto& subs = annex.subs; !subs.empty()) {
            for (const auto& aliases : subs) {
                auto id = ax_id++;
                for (const auto alias : aliases)
                    std::println(h, "{}{} = 0x{:x},", tab, alias, id);

                if (auto norm = annex.normalizer) {
                    auto sub = aliases.front();
                    auto& os = normalizers.emplace_back();
                    std::print(os, "normalizers[flags_t({}::{})] = &{}<{}::{}>;", sym.tag, sub, norm, sym.tag, sub);
                }
            }
        } else {
            if (auto norm = annex.normalizer)
                std::print(normalizers.emplace_back(), "normalizers[flags_t(Annex::Base<{}>)] = &{};", sym.tag, norm);
        }
        --tab;
        std::println(h, "{}}};\n", tab);

        std::println(outer_namespace.emplace_back(), "template<> constexpr size_t Annex::Num<plug::{}::{}> = {};", plugin, sym.tag, annex.subs.size());

        if (auto norm = annex.normalizer) {
            if (auto& subs = annex.subs; !subs.empty()) {
                std::println(h, "{}template<{}>\nconst Def* {}(const Def*, const Def*, const Def*);\n", tab, sym.tag,
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
                     sym.tag);
    }

    std::println(h, "{}\n#endif", tab);
    std::println(h, "{}}} // namespace mim\n", tab);

    std::println(h, "{}#ifndef DOXYGEN // don't include in Doxygen documentation\n", tab);
    for (const auto& [key, annex] : infos) {
        if (!annex.subs.empty()) {
            auto sym = annex.sym;
            std::println(h, "{}template<> struct fe::is_bit_enum<mim::plug::{}::{}> : std::true_type {{}};", tab,
                         sym.plugin, sym.tag);
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
    std::vector<mim::ast::AnnexInfo> annexes_with_subs;

    const auto& unordered = plugin2annexes(plugin);
    std::deque<std::pair<Sym, AnnexInfo>> infos(unordered.begin(), unordered.end());
    std::ranges::sort(infos, [&](const auto& p1, const auto& p2) { return p1.second.id.tag < p2.second.id.tag; });
    for (const auto& [key, annex] : infos) {
        const auto& sym = annex.sym;
        if (sym.plugin != plugin) continue;

        flags_t ax_id = plugin_id | (annex.id.tag << 8u);

        if (auto& subs = annex.subs; subs.empty())
            std::println(h, "{}{} = 0x{:x}", tab, sym.tag, ax_id);
        else
            annexes_with_subs.push_back(annex);
    }
    std::print(h, "\n");

    if (!annexes_with_subs.empty()) {
        plugin_t plugin_id = *Annex::mangle(plugin);
        for (const auto& annex : annexes_with_subs) {
            flags_t ax_id = plugin_id | (annex.id.tag << 8u);
            std::println(h, "class _{}_{}(IntEnum):", plugin, annex.sym.tag);
            ++tab;

            for (const auto& aliases : annex.subs) {
                auto id = ax_id++;
                for (const auto alias : aliases)
                    std::println(h, "{}{} = 0x{:x}", tab, alias, id);
            }

            --tab;
            std::println(h, "\n{}.{} = _{}_{}\n", plugin, annex.sym.tag, plugin, annex.sym.tag);
        }
    }
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
