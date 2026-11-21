#include "mim/ast/ast.h"

#include <set>

#include "mim/check.h"
#include "mim/nom.h"
#include "mim/rewrite.h"
#include "mim/rule.h"

#include "mim/ast/parser.h"
#include "mim/phase/nom_erasure.h"

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

/// Applies NomErasure to a single Rule.
/// Its new World has no annexes attached, so each Axm is copied there with its erased type.
class RuleEraser : public NomErasure {
public:
    using NomErasure::NomErasure;

    const Def* lookup(const Def* old_def) final { return Rewriter::lookup(old_def); }
};

/// Translates the IR of the `norm` rules sharing a name into the C++ normalizer of that name.
class NormGen {
public:
    /// @p plugins receives every plugin whose annexes the generated code refers to.
    NormGen(AST& ast, std::set<std::string>& plugins)
        : ast_(ast)
        , plugins_(plugins) {}

    /// Each rule becomes its own function, so a failed match can just `return nullptr` and the next rule is tried.
    /// A rule that mentions a nominal type gets an erased twin, which keeps it firing after NomErasure.
    /// A @p family's normalizer is a template over its sub-tags that only tries the rules headed by that sub-tag.
    void emit(std::ostream& os, Sym name, const std::vector<const RuleDecl*>& rules, const AnnexInfo* family) {
        auto norm  = escape(name, Cpp_Keywords);
        auto heads = std::vector<std::pair<const AnnexInfo*, sub_t>>();
        auto fn    = [&]() { return std::format("{}_{}", norm, heads.size()); };
        for (auto decl : rules) {
            auto rule = decl->def()->as<Rule>();
            heads.emplace_back(emit(os, fn(), decl, rule));
            if (RWPhase::mentions(rule,
                                  [](const Def* d) { return d->isa<Nom>() || d->isa<Name>() || d->isa<Struc>(); })) {
                auto eraser = RuleEraser(rule->world(), "nom_erasure");
                heads.emplace_back(emit(os, fn(), decl, eraser.rewrite(rule)->as<Rule>()));
            }
        }

        if (family) std::println(os, "template<{} id>", tag_name(*family));
        std::println(os,
                     "inline const Def* {}([[maybe_unused]] const Def* type, [[maybe_unused]] const Def* callee, "
                     "[[maybe_unused]] const Def* arg) {{ // generated from the `norm` rules `{}` in {}",
                     norm, name, fs::path(rules.front()->loc().src->path()).filename().string());
        for (size_t i = 0, e = heads.size(); i != e; ++i) {
            auto call = std::format("if (auto res = {}_{}(type, callee, arg)) return res;", norm, i);
            if (auto [annex, sub] = heads[i]; family && annex == family)
                std::println(os, "    if constexpr (id == {})\n        {}", sub_name(*family, sub), call);
            else
                std::println(os, "    {}", call);
        }
        std::println(os, "    return nullptr;\n}}\n");
    }

private:
    /// @returns the annex and sub-tag that head the lhs of @p rule, or `{nullptr, 0}` if no axiom does.
    std::pair<const AnnexInfo*, sub_t>
    emit(std::ostream& os, const std::string& fn, const RuleDecl* decl, const Rule* rule) {
        metas_.clear();
        names_.clear();
        bound_.clear();
        body_.str("");
        num_tmps_ = 0;

        auto [lhs, guard, rhs] = instantiate(decl, rule);
        auto app               = lhs->isa<App>();

        // TODO a non-App lhs, such as a nullary axiom, has no (callee, arg) to match against
        // TODO an Axm with a custom curry or trip counter normalizes at another spot than after its last argument
        if (!app) {
            ast_.error().e(decl->lhs()->loc(), "the left-hand side of a `norm` rule must be an application");
            return {nullptr, 0};
        }

        match(app->callee(), "callee");
        match(app->arg(), "arg");
        if (guard != guard->world().lit_tt()) line("if ({} != w.lit_tt()) return nullptr;", build(guard));
        // TODO guard against an rhs that rebuilds the lhs, which recurses into this very normalizer forever
        line("return {};", build(rhs));

        std::println(os,
                     "inline const Def* {}(const Def* type, [[maybe_unused]] const Def* callee, "
                     "[[maybe_unused]] const Def* arg) {{",
                     fn);
        std::println(os, "    [[maybe_unused]] auto& w = type->world();");
        std::print(os, "{}", body_.str());
        std::println(os, "}}\n");

        auto head = lhs;
        while (auto a = head->isa<App>())
            head = a->callee();
        if (auto axm = head->isa<Axm>(); axm && !is_meta(axm)) return annex(axm);
        return {nullptr, 0};
    }

    /// Replaces each binder by a placeholder, since eta folds `(a, b)` of the rule's single Var back into it.
    std::array<const Def*, 3> instantiate(const RuleDecl* decl, const Rule* rule) {
        auto ops = std::array{rule->lhs(), rule->guard(), rule->rhs()};
        if (auto var = rule->has_var()) {
            auto rw = VarRewriter(var, placeholder(decl->var(), rule->dom()));
            for (auto& op : ops)
                op = rw.rewrite(op);
        }
        return {ops[0]->zonk(), ops[1]->zonk(), ops[2]->zonk()};
    }

    const Def* placeholder(const Ptrn* ptrn, const Def* type) {
        auto& w = type->world();
        if (auto alias = ptrn->isa<AliasPtrn>()) return placeholder(alias->ptrn(), type);
        if (auto tuple = ptrn->isa<TuplePtrn>()) {
            auto n = tuple->num_ptrns();
            if (n == 1) return placeholder(tuple->ptrn(0), type);
            auto parts = DefVec();
            for (size_t i = 0; i != n; ++i)
                parts.emplace_back(placeholder(tuple->ptrn(i), w.component(type, parts, n, i)));
            return w.tuple(type, parts);
        }

        auto name = std::string(ptrn->dbg().sym().view());
        std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front()))) name = "m" + name;
        name += "_";
        for (auto base = name; !names_.emplace(name).second;)
            name = std::format("{}{}_", base, names_.size());

        auto meta = w.axm(type)->set(ptrn->dbg().sym());
        metas_.emplace(meta, name);
        return meta;
    }

    bool is_meta(const Def* def) const { return metas_.contains(def); }
    const std::string& meta_name(const Def* def) const { return metas_.find(def)->second; }
    bool mentions_meta(const Def* def) const {
        return RWPhase::mentions(def, [this](const Def* d) { return is_meta(d); });
    }

    void match(const Def* pat, const std::string& def) {
        if (is_meta(pat)) {
            // TODO check that def inhabits the meta variable's type
            if (bound_.emplace(pat).second)
                line("[[maybe_unused]] auto {} = {};", meta_name(pat), def);
            else
                line("if ({} != {}) return nullptr;", def, meta_name(pat));
            return;
        }

        // An implicit argument that nothing in the lhs determines.
        if (pat->isa<Hole>()) return;

        // A phase that rewrites the type of an Axm rebuilds it under the same flags, so it is no longer World::annex.
        if (auto axm = pat->isa<Axm>()) {
            if (auto [annex, sub] = this->annex(axm); annex) {
                line("if (auto axm = {}->isa<Axm>(); !axm || axm->flags() != {}) return nullptr;", def,
                     annex->subs.empty() ? std::format("Annex::base<{}>()", tag_name(*annex))
                                         : std::format("flags_t({})", sub_name(*annex, sub)));
                return;
            }
        }

        // TODO match up to normalization: e.g. a literal arg may stem from a folded application in the pattern
        if (!mentions_meta(pat)) {
            line("if ({} != {}) return nullptr;", def, build(pat));
            return;
        }

        // Projections, unlike a node check, also decompose a tuple-typed Var or App.
        if (auto tuple = pat->isa<Tuple>()) {
            auto tmp = std::format("d{}", num_tmps_++);
            auto n   = tuple->num_ops();
            line("auto {} = {};", tmp, def);
            line("if (Lit::isa({}->arity()) != {}_n) return nullptr;", tmp, n);
            for (size_t i = 0; i != n; ++i)
                match(tuple->op(i), std::format("{}->proj({}, {})", tmp, n, i));
            return;
        }

        if (pat->isa_mut()) {
            // TODO mutables in a pattern, such as a lambda over a meta variable
            ast_.error().e(pat->loc(), "mutable `{}` not yet supported in the left-hand side of a `norm` rule", pat);
            return;
        }

        auto tmp = std::format("d{}", num_tmps_++);
        line("auto {} = {};", tmp, def);
        // Some nodes keep part of their identity in their flags, e.g. the constructor index of an Inj.
        line("if ({}->node() != Node::{} || {}->flags() != 0x{:x}_u64 || {}->num_ops() != {}) return nullptr;", tmp,
             pat->node_name(), tmp, pat->flags(), tmp, pat->num_ops());
        for (size_t i = 0, e = pat->num_ops(); i != e; ++i)
            match(pat->op(i), std::format("{}->op({})", tmp, i));
    }

    std::string build(const Def* def) {
        if (is_meta(def)) {
            if (!bound_.contains(def))
                ast_.error().e(def->loc(), "meta variable `{}` is not bound by the left-hand side", def);
            return meta_name(def);
        }
        if (auto axm = def->isa<Axm>()) {
            auto [annex, sub] = this->annex(axm);
            if (annex)
                return annex->subs.empty() ? std::format("w.annex<{}>()", tag_name(*annex))
                                           : std::format("w.annex({})", sub_name(*annex, sub));
        } else if (def->isa_mut()) {
            // TODO mutables, such as lambdas or mutable annexes
        } else if (def->isa<Nat>()) {
            return "w.type_nat()";
        } else if (def->isa<Idx>()) {
            return "w.type_idx()";
        } else if (auto lit = def->isa<Lit>()) {
            auto val = lit->get();
            if (lit->type()->isa<Nat>()) return std::format("w.lit_nat({}_n)", val);
            if (auto size = Idx::isa_lit(lit->type())) return std::format("w.lit_idx({}_n, {}_n)", *size, val);
            return std::format("w.lit({}, {}_n)", build(lit->type()), val);
        } else if (auto app = def->isa<App>()) {
            return std::format("w.app({}, {})", build(app->callee()), build(app->arg()));
        } else if (auto tuple = def->isa<Tuple>()) {
            // TODO a Tuple whose type is not the Sigma of its elements' types
            return std::format("w.tuple({{{}}})", build(tuple->ops()));
        } else if (auto sigma = def->isa<Sigma>()) {
            return std::format("w.sigma({{{}}})", build(sigma->ops()));
        } else if (auto extract = def->isa<Extract>()) {
            return std::format("w.extract({}, {})", build(extract->tuple()), build(extract->index()));
        } else if (auto bot = def->isa<Bot>()) {
            return std::format("w.bot({})", build(bot->type()));
        } else if (auto top = def->isa<Top>()) {
            return std::format("w.top({})", build(top->type()));
        }

        // TODO the remaining nodes: Pi, Arr, Pack, Insert, Type, Inj, Match, ...
        ast_.error().e(def->loc(), "`{}` ({}) not yet supported in a `norm` rule", def, def->node_name());
        return "nullptr";
    }

    std::string build(Defs defs) {
        auto res = std::string();
        for (auto def : defs)
            res += (res.empty() ? "" : ", ") + build(def);
        return res;
    }

    /// The AnnexInfo that declared @p axm and its sub-tag.
    std::pair<const AnnexInfo*, sub_t> annex(const Axm* axm) {
        auto plugin = ast_.sym(Annex::demangle(axm->plugin()));
        for (const auto& [_, annex] : ast_.plugin2annexes(plugin))
            if (annex.id.tag == axm->tag()) return {&annex, axm->sub()};
        return {nullptr, 0};
    }

    std::string tag_name(const AnnexInfo& annex) {
        plugins_.emplace(annex.sym.plugin.str());
        return std::format("plug::{}::{}", annex.sym.plugin, escape(annex.sym.tag, Cpp_Keywords));
    }

    std::string sub_name(const AnnexInfo& annex, sub_t sub) {
        return std::format("{}::{}", tag_name(annex), escape(annex.subs[sub].front(), Cpp_Keywords));
    }

    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::println(body_, "    {}", std::format(fmt, std::forward<Args>(args)...));
    }

    AST& ast_;
    std::set<std::string>& plugins_;
    DefMap<std::string> metas_;
    std::set<std::string> names_;
    DefSet bound_;
    std::ostringstream body_;
    size_t num_tmps_ = 0;
};

} // namespace

void AST::bootstrap(Sym plugin, std::ostream& h) {
    auto targets = std::set<std::string>();

    // Generate the rule-based normalizers first, so an unsupported rule fails before anything is written.
    auto rule_norms = std::vector<std::pair<Sym, std::vector<const RuleDecl*>>>();
    for (auto rule : plugin2norms(plugin)) {
        auto name = rule->dbg().sym();
        auto i    = std::ranges::find(rule_norms, name, [](const auto& p) { return p.first; });
        if (i == rule_norms.end()) i = rule_norms.insert(i, {name, {}});
        i->second.emplace_back(rule);
    }
    // TODO a normalizer shared by several families would need one template per family
    auto family = [&](Sym norm) -> const AnnexInfo* {
        for (const auto& [_, annex] : plugin2annexes(plugin))
            if (annex.sym.plugin == plugin && !annex.subs.empty() && annex.normalizer.sym() == norm) return &annex;
        return nullptr;
    };
    // Bootstrap mode loads no plugin library, so no normalizer touches the rules while they are emitted.
    for (auto rule : plugin2norms(plugin))
        if (auto i = files_->find(rule->loc().src); i != files_->end()) i->second->emit(*this);
    error().ack();

    auto norms_os = std::ostringstream();
    auto norm_gen = NormGen(*this, targets);
    for (const auto& [name, rules] : rule_norms)
        norm_gen.emit(norms_os, name, rules, family(name));
    auto is_rule_norm = [&](Dbg norm) {
        return std::ranges::contains(rule_norms, norm.sym(), [](const auto& p) { return p.first; });
    };
    error().ack();
    targets.erase(plugin.str());

    auto tab = fe::Tab::spaces();
    std::println(h, "{}#pragma once\n", tab);
    std::println(h, "{}#include <mim/axm.h>", tab);
    std::println(h, "{}#include <mim/plugin.h>", tab);
    if (!rule_norms.empty()) std::println(h, "{}#include <mim/world.h>", tab);
    std::println(h, "");

    const auto& foreign = plugin2foreign_aliases(plugin);
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

        if (auto norm = annex.normalizer; norm && !is_rule_norm(norm)) {
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

    // Only here are all Annex specializations, which the generated normalizers instantiate, complete.
    if (!rule_norms.empty()) {
        std::println(h, "\nnamespace mim::plug::{} {{\n", plugin);
        std::print(h, "{}", norms_os.str());
        std::println(h, "}} // namespace mim::plug::{}", plugin);
    }
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
