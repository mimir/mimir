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
    Tab tab;
    tab.print(h, "#pragma once\n\n");
    tab.print(h, "#include <mim/axm.h>\n"
                 "#include <mim/plugin.h>\n\n");

    tab.print(h, "/// @namespace mim::plug::{} @ref {} \n", plugin, plugin);
    tab.print(h, "namespace mim {{\n");
    tab.print(h, "namespace plug::{} {{\n\n", plugin);

    plugin_t plugin_id = *Annex::mangle(plugin);
    std::vector<std::ostringstream> normalizers, outer_namespace;

    tab.print(h, "static constexpr plugin_t Plugin_Id = 0x{x};\n\n", plugin_id);

    const auto& unordered = plugin2annexes(plugin);
    std::deque<std::pair<Sym, AnnexInfo>> infos(unordered.begin(), unordered.end());
    std::ranges::sort(infos, [&](const auto& p1, const auto& p2) { return p1.second.id.tag < p2.second.id.tag; });

    // clang-format off
    for (const auto& [key, annex] : infos) {
        const auto& sym = annex.sym;
        if (sym.plugin != plugin) continue; // this is from an import

        tab.print(h, "/// @name %%{}.{}\n///@{{\n", plugin, sym.tag);
        tab.print(h, "enum class {} : flags_t {{\n", sym.tag);
        ++tab;
        flags_t ax_id = plugin_id | (annex.id.tag << 8u);

        auto& os = outer_namespace.emplace_back();
        print(os, "template<> constexpr flags_t Annex::Base<plug::{}::{}> = 0x{x};\n", plugin, sym.tag, ax_id);

        if (auto& subs = annex.subs; !subs.empty()) {
            for (const auto& aliases : subs) {
                const auto& sub = aliases.front();
                tab.print(h, "{} = 0x{x},\n", sub, ax_id++);
                for (size_t i = 1; i < aliases.size(); ++i) tab.print(h, "{} = {},\n", aliases[i], sub);

                if (auto norm = annex.normalizer) {
                    auto& os = normalizers.emplace_back();
                    print(os, "normalizers[flags_t({}::{})] = &{}<{}::{}>;", sym.tag, sub, norm, sym.tag, sub);
                }
            }
        } else {
            if (auto norm = annex.normalizer)
                print(normalizers.emplace_back(), "normalizers[flags_t(Annex::Base<{}>)] = &{};", sym.tag, norm);
        }
        --tab;
        tab.print(h, "}};\n\n");

        print(outer_namespace.emplace_back(), "template<> constexpr size_t Annex::Num<plug::{}::{}> = {};\n", plugin, sym.tag, annex.subs.size());

        if (auto norm = annex.normalizer) {
            if (auto& subs = annex.subs; !subs.empty()) {
                tab.print(h, "template<{}>\nconst Def* {}(const Def*, const Def*, const Def*);\n\n", sym.tag, norm);
            } else {
                tab.print(h, "const Def* {}(const Def*, const Def*, const Def*);\n", norm);
            }
        }
        tab.print(h, "///@}}\n\n");
    }
    // clang-format on

    if (!normalizers.empty()) {
        tab.print(h, "void register_normalizers(Normalizers& normalizers);\n\n");
        tab.print(h, "#define MIM_{}_NORMALIZER_IMPL \\\n", plugin);
        ++tab;
        tab.print(h, "void register_normalizers(Normalizers& normalizers) {{\\\n");
        ++tab;
        for (const auto& normalizer : normalizers)
            tab.print(h, "{} \\\n", normalizer.str());
        --tab;
        tab.print(h, "}}\n");
        --tab;
    }

    tab.print(h, "}} // namespace plug::{}\n\n", plugin);

    tab.print(h, "#ifndef DOXYGEN // don't include in Doxygen documentation\n\n");
    for (const auto& line : outer_namespace)
        tab.print(h, "{}", line.str());
    tab.print(h, "\n");

    // emit helpers for non-function axm
    for (const auto& [tag, ax] : infos) {
        auto sym = ax.sym;
        if ((ax.pi && *ax.pi) || sym.plugin != plugin) continue; // from function or other plugin?
        tab.print(h, "template<> struct Axm::IsANode<plug::{}::{}> {{ using type = Axm; }};\n", sym.plugin, sym.tag);
    }

    tab.print(h, "\n#endif\n");
    tab.print(h, "}} // namespace mim\n\n");

    tab.print(h, "#ifndef DOXYGEN // don't include in Doxygen documentation\n\n");
    for (const auto& [key, annex] : infos) {
        if (!annex.subs.empty()) {
            auto sym = annex.sym;
            tab.print(h, "template<> struct fe::is_bit_enum<mim::plug::{}::{}> : std::true_type {{}};\n", sym.plugin,
                      sym.tag);
        }
    }

    tab.print(h, "\n#endif\n");
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
