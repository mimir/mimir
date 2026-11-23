#include "mim/ast/ast.h"

#include <optional>
#include <regex>
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

/// Translates the `norm` rules sharing a name into the C++ normalizer of that name.
/// Each rule becomes a function that unrolls Rule::fire's Matcher for its lhs.
class NormGen {
public:
    /// @p plugins receives every plugin whose annexes the generated code refers to.
    NormGen(AST& ast, std::set<std::string>& plugins)
        : ast_(ast)
        , plugins_(plugins) {}

    /// Each rule becomes its own function, so a failed match can just `return nullptr` and the next rule is tried.
    /// A rule that mentions a nominal type gets an erased twin, which keeps it firing after NomErasure.
    /// A @p family's normalizer is a template over its sub-tags that only tries the rules headed by that sub-tag.
    void emit(std::ostream& os, Sym name, const std::vector<const LetDecl*>& rules, const AnnexInfo* family) {
        auto norm  = escape(name, Cpp_Keywords);
        auto heads = std::vector<std::pair<const AnnexInfo*, sub_t>>();
        auto fn    = [&]() { return std::format("{}_{}", norm, heads.size()); };
        for (auto decl : rules) {
            auto rule = decl->def()->as<Rule>();
            heads.emplace_back(emit(os, fn(), name, decl, rule, false));
            if (RWPhase::mentions(rule,
                                  [](const Def* d) { return d->isa<Nom>() || d->isa<Name>() || d->isa<Struc>(); }))
                heads.emplace_back(emit(os, fn(), name, decl, rule, true));
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
    /// @returns the annex and sub-tag that head the lhs of @p rule.
    std::pair<const AnnexInfo*, sub_t>
    emit(std::ostream& os, const std::string& fn, Sym name, const LetDecl* decl, const Rule* rule, bool erase) {
        names_.clear();
        used_.clear();
        bound_.clear();
        body_.str("");
        num_tmps_ = 0;

        auto eraser = std::optional<RuleEraser>();
        if (erase) rule = this->erase(eraser.emplace(rule->world(), "nom_erasure"), rule);
        auto expr = decl->value()->as<RuleExpr>();
        var_      = rule->has_var();
        if (var_) this->name(expr->meta(), var_);

        // TODO an Axm with a custom curry or trip counter normalizes at another spot than after its last argument
        auto lhs  = rule->lhs()->zonk();
        auto head = std::pair<const AnnexInfo*, sub_t>{nullptr, 0};
        if (auto [axm, curry, _] = Axm::get(lhs); axm && curry == 0) head = annex(axm);
        if (!head.first || head.first->normalizer.sym() != name) {
            ast_.error().e(expr->lhs()->loc(),
                           "the left-hand side of a `norm` rule must apply an axiom normalized by `{}`", name);
            return {nullptr, 0};
        }

        auto app = lhs->as<App>();
        match(app->callee(), "callee");
        match(app->arg(), "arg");
        line("// the type of the whole application: {}", show(lhs->type()));
        match(lhs->type(), "type");
        if (auto guard = rule->guard()->zonk(); guard != guard->world().lit_tt())
            line("if ({} != w.lit_tt()) return nullptr;", build(guard));
        // TODO guard against an rhs that rebuilds the lhs, which recurses into this very normalizer forever
        line("return {};", build(rule->rhs()->zonk()));

        auto src = std::ostringstream();
        auto tab = fe::Tab::spaces();
        expr->stream_body(tab, src);
        std::println(os, "/// `norm {}{}`", name, src.str());
        if (eraser) std::println(os, "/// with nominal types erased, for after NomErasure");
        std::println(os,
                     "inline const Def* {}(const Def* type, [[maybe_unused]] const Def* callee, "
                     "[[maybe_unused]] const Def* arg) {{",
                     fn);
        std::println(os, "    [[maybe_unused]] auto& w = type->world();");
        std::print(os, "{}", body_.str());
        std::println(os, "}}\n");
        return head;
    }

    /// Rebuilds @p rule with its nominal types erased.
    /// Rewriting the Rule as a whole would recurse, since the type of a polymorphic `norm` mentions its own Var.
    const Rule* erase(RuleEraser& eraser, const Rule* rule) {
        auto& w  = eraser.new_world();
        auto res = w.mut_rule(w.rho(w.mut_hole_type(), w.mut_hole_type()));
        res->set_meta(eraser.rewrite(rule->meta()));
        if (auto var = rule->has_var()) eraser.map(var, res->var());
        res->set_lhs(eraser.rewrite(rule->lhs()));
        res->set_guard(eraser.rewrite(rule->guard()));
        res->set_rhs(eraser.rewrite(rule->rhs()));
        return res;
    }

    /// @p def as Mim; the gid suffixes of names would make the generated header differ from build to build.
    static std::string show(const Def* def) {
        static const auto gid = std::regex("([A-Za-z][A-Za-z0-9]*)_[0-9]+");
        return std::regex_replace(std::format("{}", def), gid, "$1");
    }

    /// Names the meta variables after the binders of @p ptrn, whose value is @p def.
    void name(const Ptrn* ptrn, const Def* def) {
        if (auto alias = ptrn->isa<AliasPtrn>()) {
            fresh(def, alias->dbg().sym().view());
            return name(alias->ptrn(), def);
        }
        if (auto tuple = ptrn->isa<TuplePtrn>()) {
            auto n = tuple->num_ptrns();
            if (n == 1) return name(tuple->ptrn(0), def);
            for (size_t i = 0; i != n; ++i)
                name(tuple->ptrn(i), def->proj(n, i));
            return;
        }
        fresh(def, ptrn->dbg().sym().view());
    }

    const std::string& fresh(const Def* meta, std::string_view base) {
        auto name = std::string(base);
        std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front()))) name = "m" + name;
        name += "_";
        for (auto base = name; !used_.emplace(name).second;)
            name = std::format("{}{}_", base, used_.size());
        return names_[meta] = name;
    }

    const std::string& meta_name(const Def* meta) {
        if (auto i = names_.find(meta); i != names_.end()) return i->second;
        return fresh(meta, "v");
    }

    /// Like Matcher::is_meta: the rule's Var and its projections.
    bool is_meta(const Def* def) const {
        if (def == var_) return true;
        if (auto ex = def->isa<Extract>(); ex && Lit::isa(ex->index())) return is_meta(ex->tuple());
        return false;
    }

    bool mentions_meta(const Def* def) const {
        return var_ && RWPhase::mentions(def, [this](const Def* d) { return d == var_; });
    }

    /// Like Matcher::bound: a bound tuple also binds its projections.
    std::optional<std::string> bound(const Def* meta) {
        if (bound_.contains(meta)) return meta_name(meta);
        if (auto ex = meta->isa<Extract>())
            if (auto tuple = bound(ex->tuple()))
                return std::format("{}->proj({}, {})", *tuple, *Lit::isa(ex->tuple()->arity()), *Lit::isa(ex->index()));
        return {};
    }

    /// Like Matcher::match.
    void match(const Def* pat, const std::string& def) {
        if (is_meta(pat)) {
            if (auto val = bound(pat)) return line("if ({} != {}) return nullptr;", def, *val);
            auto name = meta_name(pat);
            line("[[maybe_unused]] auto {} = {};", name, def);
            bound_.emplace(pat);
            return match(pat->type(), name + "->type()");
        }

        // An implicit argument that nothing in the lhs determines.
        if (pat->isa<Hole>()) return;

        // A phase that retypes an Axm rebuilds it under the same flags, so it is no longer World::annex.
        if (auto axm = pat->isa<Axm>())
            return line("if (auto axm = {}->isa<Axm>(); !axm || axm->flags() != {}->flags()) return nullptr;", def,
                        build(axm));

        // TODO match up to normalization: e.g. a literal arg may stem from a folded application in the pattern
        if (!mentions_meta(pat)) return line("if ({} != {}) return nullptr;", def, build(pat));

        auto tmp = std::format("d{}", num_tmps_++);
        line("auto {} = {}; // {}", tmp, def, show(pat));

        // η: a tuple pattern matches any term of its arity by its projections.
        if (auto tuple = pat->isa<Tuple>()) {
            auto n = tuple->num_ops();
            line("if (Lit::isa({}->arity()) != {}_n) return nullptr;", tmp, n);
            for (size_t i = 0; i != n; ++i)
                match(tuple->op(i), std::format("{}->proj({}, {})", tmp, n, i));
            return;
        }

        if (pat->isa_mut())
            // TODO mutables in a pattern, such as a lambda over a meta variable
            return ast_.error().e(pat->loc(), "mutable `{}` not yet supported in a `norm` rule", pat), void();

        // The flags of a Nom are its key, which differs from World to World.
        auto flags = pat->isa<Nom>() ? nom_key(pat->as<Nom>()) : std::format("0x{:x}_u64", pat->flags());
        line("if ({}->node() != Node::{} || {}->flags() != {} || {}->num_ops() != {}) return nullptr;", tmp,
             pat->node_name(), tmp, flags, tmp, pat->num_ops());
        // The type of an App or Extract follows from its ops, and a dependent Axm type cannot be built.
        if (!pat->isa<App>() && !pat->isa<Extract>()) match(pat->type(), tmp + "->type()");
        for (size_t i = 0, e = pat->num_ops(); i != e; ++i)
            match(pat->op(i), std::format("{}->op({})", tmp, i));
    }

    /// The C++ expression that rebuilds @p def.
    std::string build(const Def* def) {
        if (is_meta(def)) {
            if (auto val = bound(def)) return *val;
            // Like Matcher::value: a tuple meta variable may be bound by its projections only.
            if (auto n = Lit::isa(def->arity()); n && *n >= 2) {
                auto elems = DefVec(*n, [&](size_t i) { return def->proj(*n, i); });
                return std::format("w.tuple({{{}}})", build(elems));
            }
            ast_.error().e(def->loc(), "meta variable `{}` is not bound by the left-hand side", def);
            return "nullptr";
        }

        if (auto axm = def->isa<Axm>()) {
            if (auto [annex, sub] = this->annex(axm); annex)
                return annex->subs.empty() ? std::format("w.annex<{}>()", tag_name(*annex))
                                           : std::format("w.annex({})", sub_name(*annex, sub));
        } else if (auto nom = def->isa<Nom>()) {
            return std::format("w.nom({}, {})", nom_key(nom), build(nom->op()));
        } else if (def->isa_mut()) {
            // TODO mutables, such as lambdas, dependent Pis, or mutable annexes
        } else if (def->isa<Nat>()) {
            return "w.type_nat()";
        } else if (def->isa<Idx>()) {
            return "w.type_idx()";
        } else if (auto type = def->isa<Type>(); type && Lit::isa(type->level())) {
            return std::format("w.type<{}>()", *Lit::isa(type->level()));
        } else if (auto lit = def->isa<Lit>()) {
            return std::format("w.lit({}, {}_n)", build(lit->type()), lit->get());
        } else if (auto app = def->isa<App>()) {
            return std::format("w.app({}, {})", build(app->callee()), build(app->arg()));
        } else if (auto pi = def->isa<Pi>()) {
            return std::format("w.pi({}, {}, {})", build(pi->dom()), build(pi->codom()), pi->is_implicit());
        } else if (auto tuple = def->isa<Tuple>()) {
            // TODO a Tuple whose type is not the Sigma of its elements' types
            return std::format("w.tuple({{{}}})", build(tuple->ops()));
        } else if (auto sigma = def->isa<Sigma>()) {
            return std::format("w.sigma({{{}}})", build(sigma->ops()));
        } else if (auto seq = def->isa<Seq>()) {
            return std::format("w.{}(Shape({}), {})", seq->isa<Pack>() ? "pack" : "arr", build(seq->op(0)),
                               build(seq->body()));
        } else if (auto extract = def->isa<Extract>()) {
            return std::format("w.extract({}, {})", build(extract->tuple()), build(extract->index()));
        } else if (auto bot = def->isa<Bot>()) {
            return std::format("w.bot({})", build(bot->type()));
        } else if (auto top = def->isa<Top>()) {
            return std::format("w.top({})", build(top->type()));
        }

        // TODO the remaining nodes: mutables, Insert, Inj, Match, ...
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
    std::pair<const AnnexInfo*, sub_t> annex(const Axm* axm) { return annex(axm->flags()); }
    std::pair<const AnnexInfo*, sub_t> annex(flags_t flags) {
        auto plugin = ast_.sym(Annex::demangle(Annex::flags2plugin(flags)));
        for (const auto& [_, annex] : ast_.plugin2annexes(plugin))
            if (annex.id.tag == Annex::flags2tag(flags)) return {&annex, Annex::flags2sub(flags)};
        return {nullptr, 0};
    }

    /// A Nom's key is a fresh gid per World, so the generated code takes it from the annex that introduces the Nom.
    std::string nom_key(const Nom* nom) {
        for (auto [flags, def] : ast_.flags2annex_def()) {
            auto path = std::string();
            for (; def->isa_mut<Lam>(); path += "->as<Lam>()->body()")
                def = def->as<Lam>()->body();
            if (auto n = def->isa<Nom>(); n && n->key() == nom->key())
                if (auto [annex, sub] = this->annex(flags); annex) {
                    auto ax = annex->subs.empty() ? std::format("w.annex<{}>()", tag_name(*annex))
                                                  : std::format("w.annex({})", sub_name(*annex, sub));
                    return std::format("{}{}->flags()", ax, path);
                }
        }
        ast_.error().e(nom->loc(), "a generated normalizer only recognizes the nominal type `{}` if it is an annex",
                       nom);
        return "0";
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
    const Var* var_ = nullptr;
    DefMap<std::string> names_;
    std::set<std::string> used_;
    DefSet bound_;
    std::ostringstream body_;
    size_t num_tmps_ = 0;
};

} // namespace

void AST::bootstrap(Sym plugin, std::ostream& h) {
    auto targets = std::set<std::string>();

    // Generate the rule-based normalizers first, so an unsupported rule fails before anything is written.
    auto rule_norms = std::vector<std::pair<Sym, std::vector<const LetDecl*>>>();
    for (auto rule : plugin2norms(plugin)) {
        auto name = rule->value()->as<RuleExpr>()->name();
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
