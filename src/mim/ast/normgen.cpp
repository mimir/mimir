#include "normgen.h"

#include <optional>
#include <regex>
#include <set>

#include "mim/nom.h"
#include "mim/rewrite.h"
#include "mim/rule.h"

#include "mim/phase/nom_erasure.h"

namespace mim::ast {

namespace {

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
        auto norm  = cpp_id(name);
        auto heads = std::vector<std::pair<const AnnexInfo*, sub_t>>();
        auto fn    = [&]() { return std::format("{}_{}", norm, heads.size()); };
        for (auto decl : rules) {
            // AST::bootstrap emitted the rule's File, so LetDecl::emit stored the Rule in Decl::def.
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
            line("if ({} != w.lit_tt()) return nullptr; // the guard: `{}`", build(guard), source(expr->guard()));
        // TODO guard against an rhs that rebuilds the lhs, which recurses into this very normalizer forever
        line("return {}; // the right-hand side: `{}`", build(rule->rhs()->zonk()), source(expr->rhs()));

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

    static std::string source(const Expr* expr) {
        auto src = std::ostringstream();
        auto tab = fe::Tab::spaces();
        expr->stream(tab, src);
        return src.str();
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
            if (auto val = bound(pat))
                return line("if ({} != {}) return nullptr; // `{}` again, so it must equal its first match", def, *val,
                            meta_show(pat));
            auto name = meta_name(pat);
            line("[[maybe_unused]] auto {} = {}; // binds `{}`", name, def, meta_show(pat));
            bound_.emplace(pat);
            return match_type(pat, name);
        }

        // An implicit argument that nothing in the lhs determines.
        if (pat->isa<Hole>()) return;

        // A phase that retypes an Axm rebuilds it under the same flags, so it is no longer World::annex.
        if (auto axm = pat->isa<Axm>())
            return line(
                "if (auto axm = {}->isa<Axm>(); !axm || axm->flags() != {}->flags()) return nullptr; // the axiom `{}`",
                def, build(axm), show(axm));

        // TODO match up to normalization: e.g. a literal arg may stem from a folded application in the pattern
        if (!mentions_meta(pat))
            return line("if ({} != {}) return nullptr; // exactly `{}`", def, build(pat), show(pat));

        auto tmp = std::format("d{}", num_tmps_++);
        line("auto {} = {}; // match `{}`", tmp, def, show(pat));

        // η: a tuple pattern matches any term of its arity by its projections.
        if (auto tuple = pat->isa<Tuple>()) {
            auto n = tuple->num_ops();
            line("if (Lit::isa({}->arity()) != {}_n) return nullptr; // {} elements, matched by projections", tmp, n,
                 n);
            for (size_t i = 0; i != n; ++i)
                match(tuple->op(i), std::format("{}->proj({}, {})", tmp, n, i));
            return;
        }

        if (pat->isa_mut())
            // TODO mutables in a pattern, such as a lambda over a meta variable
            return ast_.error().e(pat->loc(), "mutable `{}` not yet supported in a `norm` rule", pat), void();

        // The flags of a Nom are its key, which differs from World to World.
        auto flags = pat->isa<Nom>() ? nom_key(pat->as<Nom>()) : std::format("0x{:x}_u64", pat->flags());
        auto node  = std::string_view(pat->node_name());
        auto what
            = std::format("{} {} with {} operand{}", std::string_view("AEIOU").contains(node.front()) ? "an" : "a",
                          node, pat->num_ops(), pat->num_ops() == 1 ? "" : "s");
        if (pat->isa<Inj>()) what += std::format(", constructor {}", pat->flags());
        if (pat->isa<Nom>()) what = std::format("the nominal type `{}`", show(pat));
        line("if ({}->node() != Node::{} || {}->flags() != {} || {}->num_ops() != {}) return nullptr; // {}", tmp,
             pat->node_name(), tmp, flags, tmp, pat->num_ops(), what);
        // The type of an App or Extract follows from its ops, and a dependent Axm type cannot be built.
        if (!pat->isa<App>() && !pat->isa<Extract>()) match_type(pat, tmp);
        for (size_t i = 0, e = pat->num_ops(); i != e; ++i)
            match(pat->op(i), std::format("{}->op({})", tmp, i));
    }

    /// Matches the type of @p pat, whose C++ expression is @p def.
    void match_type(const Def* pat, const std::string& def) {
        if (!pat->type()) return;
        line("// the type of `{}`: {}", is_meta(pat) ? meta_show(pat) : show(pat), show(pat->type()));
        match(pat->type(), def + "->type()");
    }

    /// The name of the meta variable @p meta, as the rule spells it.
    std::string meta_show(const Def* meta) {
        if (auto i = names_.find(meta); i != names_.end()) return i->second.substr(0, i->second.size() - 1);
        return show(meta);
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
        for (auto [flags, entry] : ast_.flags2annex()) {
            auto def  = entry.def;
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
        return std::format("plug::{}::{}", annex.sym.plugin, cpp_id(annex.sym.tag));
    }

    std::string sub_name(const AnnexInfo& annex, sub_t sub) {
        return std::format("{}::{}", tag_name(annex), cpp_id(annex.subs[sub].front()));
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

void emit_norm(std::ostream& os,
               AST& ast,
               std::set<std::string>& plugins,
               Sym name,
               const std::vector<const LetDecl*>& rules,
               const AnnexInfo* family) {
    NormGen(ast, plugins).emit(os, name, rules, family);
}

} // namespace mim::ast
