#include "mim/sexpr.h"

#include <iostream>
#include <ranges>
#include <regex>
#include <sstream>

#include "mim/def.h"

#include "mim/be/emitter.h"

namespace mim::sexpr {

struct BB {
    BB()                    = default;
    BB(const BB&)           = delete;
    BB(BB&& other) noexcept = default;
    BB& operator=(BB other) noexcept { return swap(*this, other), *this; }

    std::deque<std::ostringstream>& head() { return parts[0]; }
    std::deque<std::ostringstream>& body() { return parts[1]; }
    std::deque<std::ostringstream>& tail() { return parts[2]; }

    template<class... Args>
    void body(std::format_string<Args...> s, Args&&... args) {
        std::print(body().emplace_back(), s, std::forward<Args>(args)...);
    }

    template<class... Args>
    void tail(std::format_string<Args...> s, Args&&... args) {
        std::print(tail().emplace_back(), s, std::forward<Args>(args)...);
    }

    template<class... Args>
    std::string assign(fe::Tab tab, bool slotted, std::string name, std::format_string<Args...> s, Args&&... args) {
        if (!is_assigned(name)) {
            assign(name);
            auto& os = body().emplace_back();
            if (slotted) {
                std::print(os, "\n{}(let", tab);
                ++tab;
                std::print(os, "\n{}{}", tab, name);
                std::print(os, "\n{}(scope", tab);
                ++tab;
                std::print(os, "\n{}", tab);
                std::print(os, s, std::forward<Args>(args)...);
                --tab;
                --tab;
            } else {
                std::print(os, "\n{}(let", tab);
                ++tab;
                std::print(os, "\n{}{}", tab, name);
                std::print(os, "\n{}", tab);
                std::print(os, s, std::forward<Args>(args)...);
                --tab;
            }
        }
        return name;
    }

    template<class Fn>
    std::string assign(fe::Tab tab, bool slotted, std::string name, Fn&& print_term) {
        if (!is_assigned(name)) {
            assign(name);
            auto& os = body().emplace_back();
            if (slotted) {
                std::print(os, "\n{}(let", tab);
                ++tab;
                std::print(os, "\n{}{}", tab, name);
                std::print(os, "\n{}(scope", tab);
                print_term(tab, os);
                --tab;
            } else {
                std::print(os, "\n{}(let", tab);
                ++tab;
                std::print(os, "\n{}{}", tab, name);
                --tab;
                print_term(tab, os);
            }
        }
        return name;
    }

    friend void swap(BB& a, BB& b) noexcept {
        using std::swap;
        swap(a.parts, b.parts);
        swap(a.assigned, b.assigned);
    }

    bool is_assigned(std::string name) const { return assigned.contains(name); }
    void assign(std::string name) { assigned.insert(name); }

    std::array<std::deque<std::ostringstream>, 3> parts;
    absl::flat_hash_set<std::string> assigned;
};

class Emitter : public mim::Emitter<std::string, std::string, BB, Emitter> {
public:
    using Super = mim::Emitter<std::string, std::string, BB, Emitter>;

    Emitter(World& world, std::ostream& ostream, bool typed = false, bool slotted = false)
        : Super(world, "sexpr_emitter", ostream, true) {
        typed_            = typed;
        slotted_          = slotted;
        types_enabled_    = true;
        slots_enabled_    = true;
        bindings_enabled_ = true;
    }

    bool direct_style() override { return true; }
    bool is_valid(std::string_view s) { return !s.empty(); }
    void start() override;
    void emit_imported(Lam*);
    std::string prepare();
    void emit_epilogue(Lam*);
    void finalize();

    LamSet next_lams(Lam* lam);

    void emit_decl(BB& bb, const Def* def);
    void emit_lam(Lam* parent, Lam* curr, LamSet& rec_lams);
    std::string emit_var(BB& bb, const Def* var, const Def* type, bool meta_var = false);
    std::string emit_head(BB& bb, Lam* lam, bool nested = false);
    std::string emit_cons_type(BB& bb, View<const Def*> ops);
    std::string emit_type(BB& bb, const Def* type, bool in_term = false);
    std::string emit_cons(std::vector<std::string> op_vals);
    std::string emit_node(BB& bb, const Def* def, std::string node_name, bool variadic = false, bool with_type = false);
    std::string emit_bb(BB& bb, const Def* def);

private:
    // A Def that has a name can be considered to be bound to a variable.
    // Defs that are unbound will be printed inline (by definition)
    bool is_bound(const Def* def) const { return !def->sym().empty(); }

    std::string id(const Def*, bool is_var_use = false) const;
    std::string indent(size_t tabs, std::string term);
    std::string flatten(std::string term);

    // Determines whether the symbolic expression should
    // be emitted with type annotations.
    bool typed() const { return typed_; }
    bool typed_;

    // Temporarily disable type annotations while emitting.
    // We do not want to annotate values that are emitted as part of
    // a dependent type (i.e. during calls to emit_bb from emit_type for an array shape)
    bool toggle_types() { return types_enabled_ = !types_enabled_; }
    bool types_enabled() const { return typed() && types_enabled_; }
    bool types_enabled_;

    // Determines whether the symbolic expression should
    // be emitted in a style that is compatible with slotted-egraphs.
    bool slotted() const { return slotted_; }
    bool slotted_;

    // Temporarily disable slots while emitting.
    // While slots are disabled, no identifier is prefixed with '$'
    // and no var uses are wrapped in var nodes.
    bool toggle_slots() { return slots_enabled_ = !slots_enabled_; }
    bool slots_enabled() const { return slotted() && slots_enabled_; }
    bool slots_enabled_;

    // Temporarily disable the creation/use of bindings while emitting.
    // While bindings are disabled every var use of a binding will be
    // printed as the bindings' definition instead.
    // This is useful to print a term via emit_bb() with the assumption
    // that no variables have been bound. (i.e. for printing a lambda filter)
    bool toggle_bindings() { return bindings_enabled_ = !bindings_enabled_; }
    bool bindings_enabled() const { return bindings_enabled_; }
    bool bindings_enabled_;

    // Ensures that we don't redeclare things, for example %axm.foo
    // should only be declared once.
    absl::flat_hash_set<std::string> declared_;
    bool is_declared(std::string name) { return declared_.contains(name); }

    std::ostringstream decls_;
    std::ostringstream func_decls_;
    std::ostringstream func_impls_;
};

std::string Emitter::id(const Def* def, bool is_var_use) const {
    std::string prefix = slots_enabled() ? "$" : "";
    std::string id;

    auto var_wrap = [&](std::string id) {
        auto cond_slotted = slots_enabled() && is_var_use && id.starts_with(prefix);
        auto cond_regular = !slotted() && is_var_use;
        return cond_slotted || cond_regular ? std::format("(var {})", id) : id;
    };

    // Axioms, rules, unset lambdas(imports) and externals need to be emitted without a uid
    if (def->isa<Axm>())
        id = def->sym().str();
    else if (def->isa<Rule>())
        id = def->sym().str();
    else if (def->isa<Lam>() && !def->is_set())
        id = def->sym().str();
    else if (def->is_external())
        id = def->sym().str();
    // Top-level lambdas should never be treated as slots ($-prefixed)
    else if (def->isa<Lam>() && def->is_closed())
        id = def->unique_name();
    else
        id = prefix + def->unique_name();

    return var_wrap(id);
}

// Adjusts the base indentation of a term-string like
//
// "        (app
//              foo
//              bar
//          )"
//
// to the number of tabs specified with 'tabs' (i.e. for tabs=1)
//
// "    (app
//          foo
//          bar
//      )"
//
std::string Emitter::indent(size_t tabs, std::string term) {
    std::string indent(tabs * 4, ' ');
    std::string result;
    std::string line;

    while (!term.empty() && (term.front() == '\n' || term.front() == '\r'))
        term.erase(0, 1);

    std::stringstream term_stream(term);
    size_t min_indent = term.find_first_not_of(' ');
    while (std::getline(term_stream, line)) {
        // Skips empty lines
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        result += "\n" + indent + line.substr(min_indent);
    }

    return result;
}

// Removes all indentation so a term-string like
//
// "    (app
//          foo
//          bar
//      )"
//
// becomes flattened like below
//
// "(app foo bar)"
//
std::string Emitter::flatten(std::string term) {
    term = std::regex_replace(term, std::regex("( {4})"), "");

    while (!term.empty() && (term.front() == '\n' || term.front() == '\r'))
        term.erase(0, 1);

    term = std::regex_replace(term, std::regex("(\\r|\\n)"), " ");
    return term;
}

void Emitter::start() {
    Super::start();

    ostream() << decls_.str();
    ostream() << func_decls_.str();
    ostream() << func_impls_.str();
}

void Emitter::emit_imported(Lam* lam) {
    auto bb = BB();

    const std::string lam_kind = Lam::isa_returning(lam) ? "fun" : Lam::isa_cn(lam) ? "con" : "lam";
    const std::string ext      = lam->is_external() ? "extern" : "intern";

    if (slotted()) {
        std::print(func_decls_, "(root {} {}", ext, id(lam));
        ++tab;
        if (types_enabled()) std::print(func_decls_, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
        std::print(func_decls_, "\n{}({}", tab, lam_kind);
        std::print(func_decls_, "{}", emit_var(bb, lam->var(), lam->type()->dom()));
        ++tab;
        // Since alpha-equivalent lambdas are all represented in the same eclass in slotted, we need
        // to somehow make these imports  not alpha-equivalent because our type-analysis stores
        // types on eclasses and we would otherwise be overwriting the types of other imports if we have
        // multiple. We solve this issue by putting these filler symbols "<foo-filter>" ... into the bodies.
        std::print(func_decls_, "\n{}(scope <{}-filter> <{}-body>)", tab, id(lam), id(lam));
        --tab;
        if (types_enabled()) std::print(func_decls_, ")");
        std::print(func_decls_, "))\n\n");
        --tab;

    } else {
        std::print(func_decls_, "(root {} {}", ext, id(lam));
        ++tab;
        if (types_enabled()) std::print(func_decls_, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
        std::print(func_decls_, "\n{}({}", tab, lam_kind);
        std::print(func_decls_, "{}", emit_var(bb, lam->var(), lam->type()->dom()));
        if (types_enabled()) std::print(func_decls_, ")");
        std::print(func_decls_, "))\n\n");
        --tab;
    }
}

std::string Emitter::prepare() { return root()->unique_name(); }

void Emitter::emit_epilogue(Lam* lam) {
    if (root()->sym().str().starts_with("internal_")) return;
    auto& bb = lam2bb_[lam];
    if (is_bound(lam)) bb.tail("{}", emit(lam->body()));
}

void Emitter::finalize() {
    if (root()->sym().str().starts_with("internal_")) return;
    // We don't want to emit config lams that define which rules should be emitted.
    // The rules in the body of such a lambda will be emitted into decls_
    // via emit_bb() but we don't want to emit the lambda itself.
    // We can't do this with Axm::isa because 'eqsat' is an out-of-tree plugin
    // that isn't guaranteed to have been cloned so we can't include its header file.
    else if (root()->codom()->sym().str() == "%eqsat.Config")
        return;

    LamSet rec_lams;
    auto root_lam = nest().root()->mut()->as_mut<Lam>();
    if (is_bound(root_lam)) emit_lam(root_lam, root_lam, rec_lams);
}

LamSet Emitter::next_lams(Lam* lam) {
    LamSet next_lams;
    for (auto op : lam->deps()) {
        for (auto mut : op->local_muts())
            if (auto next = nest()[mut]) {
                if (auto next_lam = next->mut()->isa<Lam>()) next_lams.insert(next_lam);
            }
    }
    return next_lams;
}

void Emitter::emit_decl(BB& bb, const Def* def) {
    if (auto axm = def->isa<Axm>()) {
        if (!world().annexes().flags2entry().contains(axm->flags()) && !is_declared(axm->sym().str())) {
            // Slots may have been disabled if we are coming from a rule declaration below
            // in which case we want to enable them for the duration of emitting the axioms' type.
            bool enable_slots = !slots_enabled();
            if (enable_slots) toggle_slots();

            if (typed()) std::print(decls_, "(@ {}\n", emit_type(bb, axm->type()));

            std::print(decls_, "(axm {}", id(axm));

            if (typed()) std::print(decls_, ")");
            std::print(decls_, ")\n\n");

            if (enable_slots) toggle_slots();

            declared_.insert(axm->sym().str());
        }
    } else if (def->isa_imm<Rule>()) {
        assert(false && "TODO no vars in immutable Rule");
    } else if (auto rule = def->isa_mut<Rule>()) {
        bool suppress_annotations = types_enabled();
        bool suppress_slots       = slots_enabled();

        if (suppress_annotations) toggle_types();
        auto meta_var_val = emit_var(bb, rule->var(), rule->dom(), true);

        if (suppress_slots) toggle_slots();
        auto lhs_val   = emit_bb(bb, rule->lhs());
        auto rhs_val   = emit_bb(bb, rule->rhs());
        auto guard_val = emit_bb(bb, rule->guard());

        if (suppress_slots) toggle_slots();
        if (suppress_annotations) toggle_types();

        std::print(decls_, "(rule {} {} {} {} {})\n\n", indent(1, id(rule)), indent(1, meta_var_val),
                   indent(1, lhs_val), indent(1, rhs_val), indent(1, guard_val));

        declared_.insert(rule->sym().str());
    }
}

void Emitter::emit_lam(Lam* parent, Lam* curr, LamSet& rec_lams) {
    // We do not want to re-emit recursively defined lambdas because it would result in an endless loop
    auto lam_node = nest()[curr];
    if (lam_node->is_recursive()) rec_lams.emplace(curr);
    assert(lam2bb_.contains(curr));
    auto& bb        = lam2bb_[curr];
    auto& parent_bb = lam2bb_[parent];

    // Lambdas that are not bound to a variable will be printed inline.
    // I.e. their definition will simply be emitted in place as in (app (lm x.x) 2)
    // Only the lambdas that are bound to a variable will be emitted here.
    const bool EMIT = is_bound(curr) && !parent_bb.is_assigned(id(curr));
    // A lambda nested inside of a top-level lambda will be wrapped with a let-binding
    // as in (root (lam x (let child (lam y y) (app child x))))
    const bool NESTED = curr != root();

    if (EMIT) {
        parent_bb.assign(id(curr));
        std::print(func_impls_, "{}", emit_head(bb, curr, NESTED));
    }

    for (auto next_lam : next_lams(curr)) {
        if (!rec_lams.contains(next_lam)) {
            // The parent of the next lam will be the parent of the current lam
            // if the current lam doesn't get emitted (is inline). This way we maintain
            // a correct child-parent relation between actually emitted lambdas.
            auto next_parent = EMIT ? curr : parent;
            emit_lam(next_parent, next_lam, rec_lams);
        }
    }

    if (EMIT) {
        int unclosed_parens = 0;

        for (auto& term : bb.body()) {
            auto opened = std::ranges::count(term.str(), '(');
            auto closed = std::ranges::count(term.str(), ')');
            unclosed_parens += opened - closed;
            std::print(func_impls_, "{}", indent(tab.indent(), term.str()));
        }

        for (auto& term : bb.tail())
            std::print(func_impls_, "{}", indent(tab.indent(), term.str()));

        std::string closing_parens(unclosed_parens, ')');
        std::print(func_impls_, "{}", closing_parens);

        // Close type annotation '@'
        if (types_enabled()) std::print(func_impls_, ")");

        --tab;
        --tab;
        if (slotted()) {
            --tab;
            if (NESTED) {
                --tab;
                // Close 'lam' and lam var 'scope'
                std::print(func_impls_, "))");
                // Close the 'let' and let 'scope' at the end of the parent lambdas' definition.
                parent_bb.tail("))");
            } else {
                // Close 'root', 'lam' and lam var 'scope'
                std::print(func_impls_, ")))\n\n");
            }

        } else if (NESTED) {
            // Close 'lam'
            std::print(func_impls_, ")");
            // Close the 'let' at the end of the parent lambdas' definition.
            parent_bb.tail(")");
        } else {
            // Close 'root' and 'lam'
            std::print(func_impls_, "))\n\n");
        }
    }
}

std::string Emitter::emit_var(BB& bb, const Def* var, const Def* type, bool meta_var) {
    std::ostringstream os;

    ++tab;
    if (slotted()) {
        // We assume that the depth of projections for rule meta vars is at most one so
        // (a: Nat, b: Bool) is okay but (a: [b: Nat]) is not.
        if (meta_var) {
            toggle_slots();
            auto projs = var->projs();
            if (projs.size() == 1 || std::ranges::all_of(projs, [](auto proj) { return proj->sym().empty(); }))
                std::print(os, "\n{}(cons (metavar {}) nil)", tab, id(var));
            else {
                std::vector<std::string> meta_vars;
                for (auto proj : projs) {
                    ++tab;
                    auto meta_var = std::format("\n{}(metavar {})", tab, id(proj));
                    --tab;
                    meta_vars.push_back(meta_var);
                }
                std::print(os, "{}", emit_cons(meta_vars));
            }
            toggle_slots();
        } else {
            std::print(os, "\n{}{}", tab, id(var));
        }
    }

    else if (meta_var) {
        auto projs = var->projs();
        if (projs.size() == 1 || std::ranges::all_of(projs, [](auto proj) { return proj->sym().empty(); }))
            std::print(os, "\n{}(metavar {})", tab, id(var));
        else {
            std::print(os, "\n{}(metavar {}", tab, id(var));
            size_t i = 0;
            for (auto proj : projs)
                std::print(os, "{}", emit_var(bb, proj, type->proj(i++), meta_var));
            std::print(os, ")");
        }
    } else {
        std::print(os, "\n{}{}", tab, id(var));
    }
    --tab;

    return os.str();
}

std::string Emitter::emit_head(BB& bb, Lam* lam, bool nested) {
    std::ostringstream os;

    const std::string lam_kind = Lam::isa_returning(lam) ? "fun" : Lam::isa_cn(lam) ? "con" : "lam";
    const std::string ext      = lam->is_external() ? "extern" : "intern";

    if (slotted()) {
        if (nested) {
            std::print(os, "\n{}(let", tab);
            ++tab;
            std::print(os, "\n{}{}", tab, id(lam));
            std::print(os, "\n{}(scope", tab);
            ++tab;
            if (types_enabled()) std::print(os, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
            std::print(os, "\n{}({}", tab, lam_kind);
        } else {
            // We toggle slot-printing to emit the lam id without a slot prefix '$'
            toggle_slots();
            std::print(os, "(root {} {}", ext, id(lam));
            toggle_slots();
            ++tab;
            if (types_enabled()) std::print(os, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
            std::print(os, "\n{}({}", tab, lam_kind);
        }

    } else if (nested) {
        std::print(os, "\n{}(let", tab);
        ++tab;
        std::print(os, "\n{}{}", tab, id(lam));
        if (types_enabled()) std::print(os, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
        std::print(os, "\n{}({}", tab, lam_kind);
    } else {
        std::print(os, "(root {} {}", ext, id(lam));
        ++tab;
        if (types_enabled()) std::print(os, "\n{}(@ {}", tab, emit_type(bb, lam->type()));
        std::print(os, "\n{}({}", tab, lam_kind);
    }

    std::print(os, "{}", emit_var(bb, lam->var(), lam->type()->dom()));

    if (slotted()) {
        ++tab;
        std::print(os, "\n{}(scope", tab);
        // Occasionally a filter will refer to variables that will only
        // start to get bound in the body of the lambda and we therefore
        // disable the use of variables for the duration of emitting the filter
        // in order to print every variable use by its definition instead.
        toggle_bindings();
        std::print(os, "{}", emit_bb(bb, lam->filter()));
        toggle_bindings();
    } else {
        std::print(os, "{}", emit_bb(bb, lam->filter()));
    }
    ++tab;

    return os.str();
}

std::string Emitter::emit_cons_type(BB& bb, View<const Def*> ops) {
    std::ostringstream os;

    if (ops.size() == 0) {
        std::print(os, "nil");
        return os.str();
    }

    size_t op_idx = 0;
    for (auto op : ops) {
        std::print(os, "(cons {} ", emit_type(bb, op));
        if (op_idx == ops.size() - 1) std::print(os, "nil");
        op_idx++;
    }

    std::string closing_brackets(ops.size(), ')');
    std::print(os, "{}", closing_brackets);

    return os.str();
}

std::string Emitter::emit_type(BB& bb, const Def* type, bool in_term /* = false*/) {
    std::ostringstream os;
    auto scope_wrap = [&](std::string val) { return slotted() ? "(scope " + val + ")" : val; };

    if (type->isa<Nat>()) {
        std::print(os, "Nat");
    } else if (auto size = Idx::isa(type)) {
        if (auto lit_size = Idx::size2bitwidth(size)) {
            switch (*lit_size) {
                case 1: return types_[type] = "Bool";
                case 8: return types_[type] = "I8";
                case 16: return types_[type] = "I16";
                case 32: return types_[type] = "I32";
                case 64: return types_[type] = "I64";
                default: break;
            }
            std::print(os, "(idx (lit {} Nat))", size);
        } else {
            std::print(os, "(idx {})", emit_type(bb, size, in_term));
        }
    } else if (auto lit = type->isa<Lit>()) {
        if (lit->type()->isa<Nat>())
            std::print(os, "(lit {} Nat)", lit);
        else if (auto size = Idx::isa(lit->type()))
            if (auto lit_size = Idx::size2bitwidth(size); lit_size && *lit_size == 1)
                std::print(os, "(lit {} Bool)", lit);
            else
                std::print(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type(), in_term));
        else
            std::print(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type(), in_term));
    } else if (auto arr = type->isa<Arr>()) {
        std::string arity_val;
        if (auto top = arr->arity()->isa<Top>()) {
            arity_val = "(top " + emit_type(bb, top->type(), in_term) + ")";
        } else {
            // We disable type annotations only if they aren't already disabled
            // because we would otherwise get annotations inside of the array type.
            // We also disable the use of bindings unless this type is emitted as part of a term.
            // In that case the array shape may refer to bound variables.
            bool suppress_annotations = types_enabled();
            if (suppress_annotations) toggle_types();
            if (!in_term) toggle_bindings();
            arity_val = flatten(emit_bb(bb, arr->arity()));
            if (suppress_annotations) toggle_types();
            if (!in_term) toggle_bindings();
        }
        std::string arr_val = arity_val + " " + emit_type(bb, arr->body(), in_term);

        if (auto var = arr->has_var()) {
            auto var_val = id(var);
            std::print(os, "(arr {} {})", var_val, scope_wrap(arr_val));
        } else {
            auto dummy_var = slotted() ? "$dummy" : "dummy";
            std::print(os, "(arr {} {})", dummy_var, scope_wrap(arr_val));
        }

    } else if (auto pi = type->isa<Pi>()) {
        std::string pi_kind = Pi::isa_implicit(pi) ? "pi*" : "pi";
        std::string doms    = emit_type(bb, pi->dom(), in_term) + " " + emit_type(bb, pi->codom(), in_term);

        if (auto var = pi->has_var()) {
            auto var_val = id(var);
            std::print(os, "({} {} {})", pi_kind, var_val, scope_wrap(doms));
        } else {
            auto dummy_var = slotted() ? "$dummy" : "dummy";
            std::print(os, "({} {} {})", pi_kind, dummy_var, scope_wrap(doms));
        }

    } else if (auto sigma = type->isa<Sigma>()) {
        std::ostringstream op_vals;
        slotted() ? op_vals << emit_cons_type(bb, sigma->ops()) + " nil"
                  : op_vals << fe::Join(
                        sigma->ops() | std::views::transform([&](auto op) { return emit_type(bb, op, in_term); }), " ");

        if (auto var = sigma->has_var()) {
            auto var_val = id(var);
            std::print(os, "(sigma {} {})", var_val, scope_wrap(op_vals.str()));
        } else {
            auto dummy_var = slotted() ? "$dummy" : "dummy";
            std::print(os, "(sigma {} {})", dummy_var, scope_wrap(op_vals.str()));
        }

    } else if (auto tuple = type->isa<Tuple>()) {
        if (slotted())
            std::print(os, "(tuple {})", emit_cons_type(bb, tuple->ops()));
        else
            std::print(
                os, "(tuple {})",
                fe::Join(tuple->ops() | std::views::transform([&](auto op) { return emit_type(bb, op, in_term); }),
                         " "));
    } else if (auto app = type->isa<App>()) {
        std::print(os, "(app {} {})", emit_type(bb, app->callee(), in_term), emit_type(bb, app->arg(), in_term));
    } else if (auto axm = type->isa<Axm>()) {
        std::print(os, "{}", id(axm));
        emit_decl(bb, axm);
    } else if (auto var = type->isa<Var>()) {
        if (var->mut()->isa<Rule>())
            std::print(os, "\n{}{}", tab, id(var));
        else
            std::print(os, "{}", id(var, true));
    } else if (auto hole = type->isa<Hole>()) {
        std::print(os, "(hole {})", emit_type(bb, hole->type(), in_term));
    } else if (auto extract = type->isa<Extract>()) {
        // Projections of rule variables are meta vars and should just be printed by name
        if (auto var = extract->tuple()->isa<Var>(); var && var->mut()->isa<Rule>())
            std::print(os, "{}", id(extract));
        else if (in_term && bb.is_assigned(id(extract)))
            std::print(os, "{}", id(extract));
        else
            std::print(os, "(extract {} {})", emit_type(bb, extract->tuple(), in_term),
                       emit_type(bb, extract->index(), in_term));
    } else if (auto mType = type->isa<Type>()) {
        std::print(os, "(type {})", emit_type(bb, mType->level(), in_term));
    } else if (type->isa<Univ>()) {
        std::print(os, "Univ");
    } else if (auto reform = type->isa<Reform>()) {
        std::print(os, "(reform {})", emit_type(bb, reform->dom(), in_term));
    } else if (auto join = type->isa<Join>()) {
        if (slotted())
            std::print(os, "(join {})", emit_cons_type(bb, join->ops()));
        else
            std::print(
                os, "(join {})",
                fe::Join(join->ops() | std::views::transform([&](auto op) { return emit_type(bb, op, in_term); }),
                         " "));
    } else if (auto meet = type->isa<Meet>()) {
        if (slotted())
            std::print(os, "(meet {})", emit_cons_type(bb, meet->ops()));
        else
            std::print(
                os, "(meet {})",
                fe::Join(meet->ops() | std::views::transform([&](auto op) { return emit_type(bb, op, in_term); }),
                         " "));
    } else if (auto bot = type->isa<Bot>()) {
        std::print(os, "(bot {})", emit_type(bb, bot->type(), in_term));
    } else if (auto top = type->isa<Top>()) {
        std::print(os, "(top {})", emit_type(bb, top->type(), in_term));
    } else {
        error("unsupported type '{}'", type);
        fe::unreachable();
    }

    return os.str();
}

// This is primarily needed because slotted-egraphs don't support
// variadic enodes (yet?) so we have to represent those as nested cons lists
// i.e. for Tuple: (tuple (cons a (cons b nil)))
std::string Emitter::emit_cons(std::vector<std::string> op_vals) {
    std::ostringstream os;

    if (op_vals.size() == 0) {
        ++tab;
        std::print(os, "\n{}nil", tab);
        --tab;
        return os.str();
    }

    size_t op_idx = 0;
    for (auto op_val : op_vals) {
        ++tab;
        std::print(os, "\n{}(cons", tab);
        ++tab;
        std::print(os, "{}", indent(tab.indent(), op_val));
        --tab;
        if (op_idx == op_vals.size() - 1) std::print(os, "\n{}nil", tab);
        --tab;

        op_idx++;
    }

    std::string closing_brackets(op_vals.size(), ')');
    std::print(os, "{}", closing_brackets);

    return os.str();
}

std::string Emitter::emit_node(BB& bb, const Def* def, std::string node_name, bool variadic, bool with_type) {
    std::ostringstream os;

    std::vector<std::string> op_vals;

    auto type_val = emit_type(bb, def->type());
    if (with_type) {
        if (!type_val.empty()) op_vals.push_back(type_val);
    }

    if (auto pack = def->isa<Pack>()) {
        if (auto var = pack->has_var()) {
            std::string var_val = " " + (slotted() ? id(var) + " (scope" : id(var));
            op_vals.push_back(var_val);
        } else {
            std::string var_val = slotted() ? " $dummy (scope" : " dummy";
            op_vals.push_back(var_val);
        }
        if (auto arity_val = emit_bb(bb, pack->arity()); !arity_val.empty()) op_vals.push_back(arity_val);
    }

    if (auto proxy = def->isa<Proxy>()) {
        std::ostringstream pass;
        std::ostringstream tag;
        std::print(pass, "\n{}", proxy->pass());
        std::print(tag, "\n{}", proxy->tag());
        op_vals.push_back(pass.str());
        op_vals.push_back(tag.str());
    }

    for (auto op : def->ops())
        if (auto op_val = emit_bb(bb, op); !op_val.empty()) op_vals.push_back(op_val);

    if (is_bound(def) && bindings_enabled()) {
        bb.assign(tab, slotted(), id(def), [&](fe::Tab tab, auto& os) {
            ++tab;
            if (types_enabled()) std::print(os, "\n{}(@ {}", tab, type_val);
            std::print(os, "\n{}({}", tab, node_name);

            if (slotted() && variadic)
                std::print(os, "{}", emit_cons(op_vals));
            else {
                ++tab;
                for (auto op_val : op_vals)
                    std::print(os, "{}", indent(tab.indent(), op_val));
                --tab;
            }

            // Close the packs' var 'scope'
            if (slotted() && def->isa<Pack>()) std::print(os, ")");

            std::print(os, ")");
            if (types_enabled()) std::print(os, ")");
            --tab;
        });
        std::print(os, "\n{}{}", tab, id(def, true));

    } else {
        std::print(os, "\n{}({}", tab, node_name);

        if (slotted() && variadic)
            std::print(os, "{}", emit_cons(op_vals));
        else
            for (auto op_val : op_vals)
                std::print(os, "{}", op_val);

        // Close the packs' var 'scope'
        if (slotted() && def->isa<Pack>()) std::print(os, ")");

        std::print(os, ")");
    }

    return os.str();
}

std::string Emitter::emit_bb(BB& bb, const Def* def) {
    std::ostringstream os;

    ++tab;
    if (def->type()->isa<Type>() || def->type()->isa<Univ>()) {
        std::print(os, "\n{}{}", tab, emit_type(bb, def, true));
        // Short circuit because we probably don't want to type
        // annotate a type (or do we?)
        --tab;
        return os.str();
    }

    // We don't annotate axioms since this makes the sexpr's extremely cluttered and the axioms
    // will already be emitted separately with an annotation.
    if (types_enabled() && !def->isa<Axm>()) std::print(os, "\n{}(@ {}", tab, emit_type(bb, def->type()));

    if (def->isa_imm<Lam>()) {
        assert(false && "TODO immutable lam inline");
    } else if (auto lam = def->isa_mut<Lam>()) {
        if (is_bound(lam))
            std::print(os, "\n{}{}", tab, id(lam, true));
        else {
            auto lam_kind = Lam::isa_returning(lam) ? "fun" : Lam::isa_cn(lam) ? "con" : "lam";
            if (slotted()) {
                std::print(os, "\n{}({}", tab, lam_kind);
                std::print(os, "{}", emit_var(bb, lam->var(), lam->var()->type()));
                ++tab;
                std::print(os, "\n{}(scope", tab);
                std::print(os, "{}", emit_bb(bb, lam->filter()));
                std::print(os, "{}", emit_bb(bb, lam->body()));
                --tab;
                std::print(os, "))");
            } else {
                std::print(os, "\n{}({}", tab, lam_kind);
                std::print(os, "\n{}{}", tab, emit_var(bb, lam->var(), lam->var()->type()));
                std::print(os, "{}", emit_bb(bb, lam->filter()));
                std::print(os, "{}", emit_bb(bb, lam->body()));
                std::print(os, ")");
            }
        }
    } else if (auto lit = def->isa<Lit>()) {
        if (lit->type()->isa<Nat>())
            std::print(os, "\n{}(lit {} Nat)", tab, lit);
        else if (auto size = Idx::isa(lit->type()))
            if (auto lit_size = Idx::size2bitwidth(size); lit_size && *lit_size == 1)
                std::print(os, "\n{}(lit {} Bool)", tab, lit);
            else
                std::print(os, "\n{}(lit {} {})", tab, lit->get(), emit_type(bb, lit->type()));
        else
            std::print(os, "\n{}(lit {} {})", tab, lit->get(), emit_type(bb, lit->type()));
    } else if (auto tuple = def->isa<Tuple>()) {
        std::print(os, "{}", emit_node(bb, tuple, "tuple", true));
    } else if (auto pack = def->isa<Pack>()) {
        std::print(os, "{}", emit_node(bb, pack, "pack"));
    } else if (auto extract = def->isa<Extract>()) {
        // Projections of rule variables are meta vars and should just be printed by name
        if (auto var = extract->tuple()->isa<Var>(); var && var->mut()->isa<Rule>())
            std::print(os, "\n{}{}", tab, id(extract));
        else
            std::print(os, "{}", emit_node(bb, extract, "extract"));
    } else if (auto insert = def->isa<Insert>()) {
        std::print(os, "{}", emit_node(bb, insert, "insert"));
    } else if (auto var = def->isa<Var>()) {
        if (var->mut()->isa<Rule>())
            std::print(os, "\n{}{}", tab, id(var));
        else
            std::print(os, "\n{}{}", tab, id(var, true));
    } else if (auto app = def->isa<App>()) {
        std::print(os, "{}", emit_node(bb, app, "app"));
    } else if (auto axm = def->isa<Axm>()) {
        std::print(os, "\n{}{}", tab, id(axm));
        emit_decl(bb, axm);
    } else if (auto bot = def->isa<Bot>()) {
        if (is_bound(bot)) {
            bb.assign(tab, slotted(), id(bot), "(bot {})", emit_type(bb, bot->type()));
            std::print(os, "\n{}{}", tab, id(bot, true));
        } else {
            std::print(os, "\n{}(bot {})", tab, emit_type(bb, bot->type()));
        }
    } else if (auto top = def->isa<Top>()) {
        if (is_bound(top)) {
            bb.assign(tab, slotted(), id(top), "(top {})", emit_type(bb, top->type()));
            std::print(os, "\n{}{}", tab, id(top, true));
        } else {
            std::print(os, "\n{}(top {})", tab, emit_type(bb, top->type()));
        }
    } else if (auto rule = def->isa<Rule>()) {
        std::print(os, "\n{}{}", tab, id(rule, true));
        emit_decl(bb, rule);
    } else if (auto inj = def->isa<Inj>()) {
        std::print(os, "{}", emit_node(bb, inj, "inj", false, true));
    } else if (auto merge = def->isa<Merge>()) {
        std::print(os, "{}", emit_node(bb, merge, "merge", true, true));
    } else if (auto match = def->isa<Match>()) {
        std::print(os, "{}", emit_node(bb, match, "match", true));
    } else if (auto proxy = def->isa<Proxy>()) {
        std::print(os, "{}", emit_node(bb, proxy, "proxy", true, true));
    } else if (auto hole = def->isa<Hole>()) {
        std::print(os, "\n{}(hole {})", tab, emit_type(bb, hole->type()));
    } else {
        error("Unhandled Def in SExpr backend: {} : {}", def, def->type());
        fe::unreachable();
    }

    if (types_enabled() && !def->isa<Axm>()) std::print(os, ")");
    --tab;

    return os.str();
}

void emit(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream);
    emitter.run();
}

void emit_typed(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream, true);
    emitter.run();
}

void emit_slotted(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream, false, true);
    emitter.run();
}

void emit_slotted_typed(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream, true, true);
    emitter.run();
}

} // namespace mim::sexpr
