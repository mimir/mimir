#include <iostream>
#include <regex>
#include <sstream>

#include <mim/plug/core/be/sexpr.h>
#include <mim/plug/math/math.h>

#include "mim/def.h"

#include "mim/be/emitter.h"
#include "mim/util/print.h"

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
    void body(const char* s, Args&&... args) {
        print(body().emplace_back(), s, std::forward<Args>(args)...);
    }

    template<class... Args>
    void tail(const char* s, Args&&... args) {
        print(tail().emplace_back(), s, std::forward<Args>(args)...);
    }

    template<class... Args>
    std::string assign(Tab tab, bool slotted, std::string name, const char* s, Args&&... args) {
        if (!assigned.contains(name)) {
            assigned.insert(name);
            auto& os = body().emplace_back();
            if (slotted) {
                tab.lnprint(os, "(let");
                ++tab;
                tab.lnprint(os, "{}", name);
                tab.lnprint(os, "(scope");
                ++tab;
                tab.lnprint(os, s, std::forward<Args&&>(args)...);
                --tab;
                --tab;
            } else {
                tab.lnprint(os, "(let");
                ++tab;
                tab.lnprint(os, "{}", name);
                tab.lnprint(os, s, std::forward<Args&&>(args)...);
                --tab;
            }
        }
        return name;
    }

    template<class Fn>
    std::string assign(Tab tab, bool slotted, std::string name, Fn&& print_term) {
        if (!assigned.contains(name)) {
            assigned.insert(name);
            auto& os = body().emplace_back();
            if (slotted) {
                tab.lnprint(os, "(let");
                ++tab;
                tab.lnprint(os, "{}", name);
                tab.lnprint(os, "(scope");
                print_term(tab, os);
                --tab;
            } else {
                tab.lnprint(os, "(let");
                ++tab;
                tab.lnprint(os, "{}", name);
                --tab;
                print_term(tab, os);
            }
        }
        return name;
    }

    friend void swap(BB& a, BB& b) noexcept {
        using std::swap;
        swap(a.parts, b.parts);
    }

    std::array<std::deque<std::ostringstream>, 3> parts;
    std::set<std::string> assigned;
};

class Emitter : public mim::Emitter<std::string, std::string, BB, Emitter> {
public:
    using Super = mim::Emitter<std::string, std::string, BB, Emitter>;

    Emitter(World& world, std::ostream& ostream, bool slotted = false)
        : Super(world, "sexpr_emitter", ostream) {
        slotted_       = slotted;
        slots_enabled_ = true;
        vars_enabled_  = true;
    }

    bool is_valid(std::string_view s) { return !s.empty(); }
    void start() override;
    void emit_imported(Lam*);
    std::string prepare();
    void emit_epilogue(Lam*);
    void finalize();

    using LamSet = std::set<Lam*>;
    LamSet next_lams(Lam* lam);

    void emit_lam(Lam* lam, LamSet& rec_lams);
    std::string emit_var(BB& bb, const Def* var, const Def* type, bool meta_var = false);
    std::string emit_head(BB& bb, Lam* lam, bool as_binding = false);
    std::string emit_cons_type(BB& bb, View<const Def*> ops);
    std::string emit_type(BB& bb, const Def* type);
    std::string emit_cons(std::vector<std::string> op_vals);
    std::string emit_node(BB& bb, const Def* def, std::string node_name, bool variadic = false, bool with_type = false);
    std::string emit_bb(BB& bb, const Def* def);

private:
    std::string id(const Def*, bool is_var_use = false) const;
    std::string indent(size_t tabs, std::string term);
    std::string flatten(std::string term);

    // Determines whether the symbolic expression should
    // be emitted in a style that is compatible with slotted-egraphs.
    bool slotted() const { return slotted_; }
    bool slotted_;

    // Temporarily disable slots while emitting.
    // While slots are disabled, no identifier is prefixed with '$'
    // and no var uses are wrapped in var nodes.
    bool toggle_slots() { return slots_enabled_ = !slots_enabled_; }
    bool slots_enabled_;

    // Temporarily disable the use of variables while emitting.
    // While vars are disabled every var use will be printed via
    // the vars' definition instead of its name.
    // This is useful to print a term via emit_bb() with the assumption
    // that no variables have been bound. (i.e. for printing a lambda filter)
    bool toggle_vars() { return vars_enabled_ = !vars_enabled_; }
    bool vars_enabled_;

    std::ostringstream decls_;
    std::ostringstream func_decls_;
    std::ostringstream func_impls_;
};

std::string Emitter::id(const Def* def, bool is_var_use) const {
    std::string prefix = slotted() && slots_enabled_ ? "$" : "";
    std::string id;

    // In slotted-egraphs variable-uses need to be explicitly wrapped in a var node i.e. in λx.x (lam $x (var $x))
    auto var_wrap
        = [&](std::string id) { return slotted() && is_var_use && id.starts_with('$') ? "(var " + id + ")" : id; };

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

    const std::string ext = lam->is_external() ? "extern" : "intern";

    if (slotted()) {
        print(func_decls_, "(root {} {}", ext, id(lam));
        ++tab;
        tab.lnprint(func_decls_, "(con");
        print(func_decls_, "{}", emit_var(bb, lam->var(), lam->type()->dom()));
        ++tab;
        tab.lnprint(func_decls_, "(scope nil nil)");
        --tab;
        print(func_decls_, "))\n\n");
        --tab;

    } else {
        print(func_decls_, "(con {} {}", ext, id(lam));
        print(func_decls_, "{}", emit_var(bb, lam->var(), lam->type()->dom()));
        print(func_decls_, ")\n\n");
    }
}

std::string Emitter::prepare() { return root()->unique_name(); }

void Emitter::emit_epilogue(Lam* lam) {
    auto& bb = lam2bb_[lam];
    bb.tail("{}", emit(lam->body()));
}

void Emitter::finalize() {
    if (root()->sym().str() == "_fallback_compile") return;
    // We don't want to emit config lams that define which rules should be emitted.
    // The rules in the body of such a lambda will be emitted into decls_
    // via emit_bb() but we don't want to emit the lambda itself.
    // We can't do this with Axm::isa because 'eqsat' is an out-of-tree plugin
    // that isn't guaranteed to have been cloned so we can't include its header file.
    else if (root()->ret_dom()->sym().str() == "%eqsat.Rules")
        return;

    LamSet rec_lams;
    auto root_lam = nest().root()->mut()->as_mut<Lam>();
    emit_lam(root_lam, rec_lams);
}

std::set<Lam*> Emitter::next_lams(Lam* lam) {
    std::set<Lam*> next_lams;
    for (auto op : lam->deps()) {
        for (auto mut : op->local_muts())
            if (auto next = nest()[mut]) {
                if (auto next_lam = next->mut()->isa<Lam>()) next_lams.insert(next_lam);
            }
    }
    return next_lams;
}

void Emitter::emit_lam(Lam* lam, LamSet& rec_lams) {
    // We do not want to re-emit recursively defined lambdas because it would result in an endless loop
    auto lam_node = nest()[lam];
    if (lam_node->is_recursive()) rec_lams.emplace(lam);
    assert(lam2bb_.contains(lam));
    auto& bb = lam2bb_[lam];

    bool as_binding = !lam->is_closed();
    print(func_impls_, "{}", emit_head(bb, lam, as_binding));

    ++tab;
    // Keeps count of parentheses opened by let-bindings that need to be closed later on
    int unclosed_parens = 0;
    for (auto next_lam : next_lams(lam)) {
        if (!rec_lams.contains(next_lam)) {
            emit_lam(next_lam, rec_lams);
            // A lambda-binding in slotted opens two parentheses, one for the let-node and one for its scope
            unclosed_parens += slotted() ? 2 : 1;
        }
    }

    for (auto& term : bb.body()) {
        auto opened = std::ranges::count(term.str(), '(');
        auto closed = std::ranges::count(term.str(), ')');
        unclosed_parens += opened - closed;
        print(func_impls_, "{}", indent(tab.indent(), term.str()));
    }

    for (auto& term : bb.tail())
        print(func_impls_, "{}", indent(tab.indent(), term.str()));

    std::string closing_parens(unclosed_parens, ')');
    print(func_impls_, "{}", closing_parens);
    --tab;

    if (slotted()) {
        --tab;
        --tab;
        if (as_binding) {
            --tab;
            print(func_impls_, "))");
        } else {
            print(func_impls_, ")))\n\n");
        }

    } else if (as_binding) {
        --tab;
        print(func_impls_, ")");
    } else {
        print(func_impls_, ")\n\n");
    }
}

std::string Emitter::emit_var(BB& bb, const Def* var, const Def* type, bool meta_var) {
    std::ostringstream os;

    ++tab;
    // We assume that the depth of projections for rule meta vars is at most one so
    // (a: Nat, b: Bool) is okay but (a: [b: Nat]) is not.
    if (slotted() && meta_var) {
        auto projs = var->projs();
        if (projs.size() == 1 || std::ranges::all_of(projs, [](auto proj) { return proj->sym().empty(); }))
            tab.lnprint(os, "(cons (metavar {} {}) nil)", id(var), emit_type(bb, type));
        else {
            size_t i = 0;
            std::vector<std::string> meta_vars;
            for (auto proj : projs) {
                std::ostringstream meta_var;
                ++tab;
                tab.lnprint(meta_var, "(metavar");
                print(meta_var, "{}", emit_bb(bb, proj));
                ++tab;
                tab.lnprint(meta_var, "{})", emit_type(bb, type->proj(i++)));
                --tab;
                --tab;
                meta_vars.push_back(meta_var.str());
            }
            print(os, "{}", emit_cons(meta_vars));
        }
    }

    else if (slotted()) {
        tab.lnprint(os, "{}", emit_type(bb, type));
        tab.lnprint(os, "{}", id(var));
    }

    else {
        auto projs = var->projs();
        if (projs.size() == 1 || std::ranges::all_of(projs, [](auto proj) { return proj->sym().empty(); }))
            tab.lnprint(os, "(var {} {})", id(var), emit_type(bb, type));
        else {
            tab.lnprint(os, "(var {}", id(var));
            size_t i = 0;
            for (auto proj : projs)
                print(os, "{}", emit_var(bb, proj, type->proj(i++)));
            ++tab;
            tab.lnprint(os, "{})", emit_type(bb, type));
            --tab;
        }
    }
    --tab;

    return os.str();
}

std::string Emitter::emit_head(BB& bb, Lam* lam, bool as_binding) {
    std::ostringstream os;

    // TODO: Is there ever a case where we would be emitting a lambda instead of a continuation
    // since the assertions in the Emitter visit() method seem to make this impossible?
    const std::string lam_kind = lam->isa_cn(lam) ? "con" : "lam";
    const std::string ext      = lam->is_external() ? "extern" : "intern";

    if (slotted()) {
        if (as_binding) {
            tab.lnprint(os, "(let");
            ++tab;
            tab.lnprint(os, "{}", id(lam));
            tab.lnprint(os, "(scope");
            ++tab;
            tab.lnprint(os, "({}", lam_kind);
        } else {
            // We toggle slot-printing to emit the lam id without a slot prefix '$'
            toggle_slots();
            print(os, "(root {} {}", ext, id(lam));
            ++tab;
            tab.lnprint(os, "({}", lam_kind);
            toggle_slots();
        }

    } else if (as_binding) {
        tab.lnprint(os, "(let");
        ++tab;
        tab.lnprint(os, "{}", id(lam));
        tab.lnprint(os, "({} {} {}", lam_kind, ext, id(lam));
    } else {
        print(os, "({} {} {}", lam_kind, ext, id(lam));
    }

    print(os, "{}", emit_var(bb, lam->var(), lam->type()->dom()));

    if (!lam->isa_cn(lam)) {
        ++tab;
        tab.lnprint(os, "{}", emit_type(bb, lam->type()->codom()));
        --tab;
    }

    if (slotted()) {
        ++tab;
        tab.lnprint(os, "(scope");
        // Occasionally a filter will refer to variables that will only
        // start to get bound in the body of the lambda and we therefore
        // disable the use of variables for the duration of emitting the filter
        // in order to print every variable use by its definition instead.
        toggle_vars();
        print(os, "{}", emit_bb(bb, lam->filter()));
        toggle_vars();
    } else {
        print(os, "{}", emit_bb(bb, lam->filter()));
    }

    return os.str();
}

std::string Emitter::emit_cons_type(BB& bb, View<const Def*> ops) {
    std::ostringstream os;

    if (ops.size() == 0) {
        print(os, "nil");
        return os.str();
    }

    size_t op_idx = 0;
    for (auto op : ops) {
        print(os, "(cons {} ", emit_type(bb, op));
        if (op_idx == ops.size() - 1) print(os, "nil");
        op_idx++;
    }

    std::string closing_brackets(ops.size(), ')');
    print(os, "{}", closing_brackets);

    return os.str();
}

std::string Emitter::emit_type(BB& bb, const Def* type) {
    if (auto i = types_.find(type); i != types_.end()) return i->second;

    std::ostringstream os;
    if (type->isa<Nat>()) {
        print(os, "Nat");
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
        }
        print(os, "(idx (lit {} Nat))", size);
    } else if (auto lit = type->isa<Lit>()) {
        if (lit->type()->isa<Nat>())
            print(os, "(lit {} Nat)", lit);
        else if (auto size = Idx::isa(lit->type()))
            if (auto lit_size = Idx::size2bitwidth(size); lit_size && *lit_size == 1)
                print(os, "(lit {} Bool)", lit);
            else
                print(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type()));
        else
            print(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type()));
    } else if (auto arr = type->isa<Arr>()) {
        if (auto arity = Lit::isa(arr->arity())) {
            u64 size = *arity;
            print(os, "(arr (lit {} Nat) {})", size, emit_type(bb, arr->body()));
        } else if (auto top = arr->arity()->isa<Top>()) {
            print(os, "(arr (top {}) {})", emit_type(bb, top->type()), emit_type(bb, arr->body()));
        } else {
            print(os, "(arr {} {})", flatten(emit_bb(bb, arr->arity())), emit_type(bb, arr->body()));
        }
    } else if (auto pi = type->isa<Pi>()) {
        if (Pi::isa_cn(pi))
            print(os, "(cn {})", emit_type(bb, pi->dom()));
        else
            print(os, "(pi {} {})", emit_type(bb, pi->dom()), emit_type(bb, pi->codom()));
    } else if (auto sigma = type->isa<Sigma>()) {
        if (slotted())
            print(os, "(sigma {})", emit_cons_type(bb, sigma->ops()));
        else
            print(os, "(sigma { })", Elem(sigma->ops(), [&](auto op) { print(os, "{}", emit_type(bb, op)); }));
    } else if (auto tuple = type->isa<Tuple>()) {
        if (slotted())
            print(os, "(tuple {})", emit_cons_type(bb, tuple->ops()));
        else
            print(os, "(tuple { })", Elem(tuple->ops(), [&](auto op) { print(os, "{}", emit_type(bb, op)); }));
    } else if (auto app = type->isa<App>()) {
        print(os, "(app {} {})", emit_type(bb, app->callee()), emit_type(bb, app->arg()));
    } else if (auto axm = type->isa<Axm>()) {
        print(os, "{}", id(axm));
        if (!world().flags2annex().contains(axm->flags()))
            print(decls_, "(axm {} {})\n\n", id(axm), emit_type(bb, axm->type()));
    } else if (auto var = type->isa<Var>()) {
        print(os, "{}", id(var, true));
    } else if (auto hole = type->isa<Hole>()) {
        print(os, "(hole {})", id(hole));
    } else if (auto extract = type->isa<Extract>()) {
        auto tuple = extract->tuple();
        auto index = extract->index();
        // See explanation for the same thing in emit_bb
        auto is_nested_proj = false;
        if (tuple->isa<Extract>() && Lit::isa(index)) {
            auto curr_tuple = tuple;
            auto curr_index = index;
            while (curr_tuple && curr_index) {
                if (curr_tuple->isa<Extract>() && Lit::isa(curr_index)) {
                    auto extract = curr_tuple->as<Extract>();
                    curr_tuple   = extract->tuple();
                    curr_index   = extract->index();
                    continue;
                } else if (curr_tuple->isa<Var>() && Lit::isa(curr_index)) {
                    is_nested_proj = true;
                }
                break;
            }
        }
        if (!slotted() && ((Lit::isa(index) && tuple->isa<Var>()) || is_nested_proj))
            print(os, "{}", id(extract));
        else
            print(os, "(extract {} {})", emit_type(bb, tuple), emit_type(bb, index));
    } else if (auto mType = type->isa<Type>()) {
        print(os, "(type {})", emit_type(bb, mType->level()));
    } else if (type->isa<Univ>()) {
        print(os, "Univ");
    } else if (auto reform = type->isa<Reform>()) {
        print(os, "(reform {})", emit_type(bb, reform->dom()));
    } else if (auto join = type->isa<Join>()) {
        if (slotted())
            print(os, "(join {})", emit_cons_type(bb, join->ops()));
        else
            print(os, "(join { })", Elem(join->ops(), [&](auto op) { print(os, "{}", emit_type(bb, op)); }));
    } else if (auto meet = type->isa<Meet>()) {
        if (slotted())
            print(os, "(meet {})", emit_cons_type(bb, meet->ops()));
        else
            print(os, "(meet { })", Elem(meet->ops(), [&](auto op) { print(os, "{}", emit_type(bb, op)); }));
    } else if (auto bot = type->isa<Bot>()) {
        print(os, "(bot {})", emit_type(bb, bot->type()));
    } else if (auto top = type->isa<Top>()) {
        print(os, "(top {})", emit_type(bb, top->type()));
    } else {
        error("unsupported type '{}'", type);
        fe::unreachable();
    }

    return types_[type] = os.str();
}

// This is primarily needed because slotted-egraphs don't support
// variadic enodes (yet?) so we have to represent those as nested cons lists
// i.e. for Tuple: (tuple (cons a (cons b nil)))
std::string Emitter::emit_cons(std::vector<std::string> op_vals) {
    std::ostringstream os;

    if (op_vals.size() == 0) {
        ++tab;
        tab.lnprint(os, "nil");
        --tab;
        return os.str();
    }

    size_t op_idx = 0;
    for (auto op_val : op_vals) {
        ++tab;
        tab.lnprint(os, "(cons");
        ++tab;
        print(os, "{}", indent(tab.indent(), op_val));
        --tab;
        if (op_idx == op_vals.size() - 1) tab.lnprint(os, "nil");
        --tab;

        op_idx++;
    }

    std::string closing_brackets(op_vals.size(), ')');
    print(os, "{}", closing_brackets);

    return os.str();
}

std::string Emitter::emit_node(BB& bb, const Def* def, std::string node_name, bool variadic, bool with_type) {
    std::ostringstream os;

    std::vector<std::string> op_vals;

    if (with_type) {
        if (auto type_val = emit_bb(bb, def->type()); !type_val.empty()) op_vals.push_back(type_val);
    }

    // This is a bit of an edge case? because the ops of a pack don't contain its arity
    if (auto pack = def->isa<Pack>())
        if (auto arity_val = emit_bb(bb, pack->arity()); !arity_val.empty()) op_vals.push_back(arity_val);

    for (auto op : def->ops())
        if (auto op_val = emit_bb(bb, op); !op_val.empty()) op_vals.push_back(op_val);

    if (!def->sym().empty() && vars_enabled_) {
        // 1) Emits a let-binding to the lambda body() and then emits the name of the binding in the lambda tail()

        bb.assign(tab, slotted(), id(def), [&](Tab tab, auto& os) {
            ++tab;
            tab.lnprint(os, "({}", node_name);

            if (slotted() && variadic)
                print(os, "{}", emit_cons(op_vals));
            else {
                ++tab;
                for (auto op_val : op_vals)
                    print(os, "{}", indent(tab.indent(), op_val));
                --tab;
            }

            print(os, ")");
            --tab;
        });
        tab.lnprint(os, "{}", id(def, true));

    } else {
        // 2) Directly emits the definition of a Def to the lambda tail()

        tab.lnprint(os, "({}", node_name);

        if (slotted() && variadic)
            print(os, "{}", emit_cons(op_vals));
        else
            for (auto op_val : op_vals)
                print(os, "{}", op_val);

        print(os, ")");
    }

    return os.str();
}

std::string Emitter::emit_bb(BB& bb, const Def* def) {
    std::ostringstream os;

    ++tab;
    if (def->type()->isa<Type>() || def->type()->isa<Univ>()) {
        tab.lnprint(os, "{}", emit_type(bb, def));

    } else if (auto lam = def->isa<Lam>()) {
        tab.lnprint(os, "{}", id(lam, true));

    } else if (auto lit = def->isa<Lit>()) {
        if (lit->type()->isa<Nat>())
            tab.lnprint(os, "(lit {} Nat)", lit);
        else if (auto size = Idx::isa(lit->type()))
            if (auto lit_size = Idx::size2bitwidth(size); lit_size && *lit_size == 1)
                tab.lnprint(os, "(lit {} Bool)", lit);
            else
                tab.lnprint(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type()));
        else
            tab.lnprint(os, "(lit {} {})", lit->get(), emit_type(bb, lit->type()));

    } else if (auto tuple = def->isa<Tuple>()) {
        print(os, "{}", emit_node(bb, tuple, "tuple", true));

    } else if (auto pack = def->isa<Pack>()) {
        print(os, "{}", emit_node(bb, pack, "pack"));

    } else if (auto extract = def->isa<Extract>()) {
        auto tuple = extract->tuple();
        auto index = extract->index();
        // 'tuple' is another extract if we have for example two nested, sigma-typed variables
        // and we are trying to print a named projection of the inner variable. ('baz' in the example below)
        // ex.:  (var foo (sigma (var bar (sigma (var baz Nat)))))
        // In this example, we have an extract where the tuple: 'bar' is another extract from 'foo'.
        auto is_nested_proj = false;
        if (tuple->isa<Extract>() && Lit::isa(index)) {
            auto curr_tuple = tuple;
            auto curr_index = index;
            while (curr_tuple && curr_index) {
                if (curr_tuple->isa<Extract>() && Lit::isa(curr_index)) {
                    auto extract = curr_tuple->as<Extract>();
                    curr_tuple   = extract->tuple();
                    curr_index   = extract->index();
                    continue;
                } else if (curr_tuple->isa<Var>() && Lit::isa(curr_index)) {
                    is_nested_proj = true;
                }
                break;
            }
        }
        if (!slotted() && ((Lit::isa(index) && tuple->isa<Var>()) || is_nested_proj))
            tab.lnprint(os, "{}", id(extract));
        else
            print(os, "{}", emit_node(bb, extract, "extract"));

    } else if (auto insert = def->isa<Insert>()) {
        print(os, "{}", emit_node(bb, insert, "insert"));

    } else if (auto var = def->isa<Var>()) {
        tab.lnprint(os, "{}", id(var, true));

    } else if (auto app = def->isa<App>()) {
        print(os, "{}", emit_node(bb, app, "app"));

    } else if (auto axm = def->isa<Axm>()) {
        tab.lnprint(os, "{}", id(axm));
        if (!world().flags2annex().contains(axm->flags()))
            print(decls_, "(axm {} {})\n\n", id(axm), emit_type(bb, axm->type()));

    } else if (auto bot = def->isa<Bot>()) {
        if (bot->sym().empty())
            tab.lnprint(os, "(bot {})", emit_type(bb, bot->type()));
        else {
            bb.assign(tab, slotted(), id(bot), "(bot {})", emit_type(bb, bot->type()));
            tab.lnprint(os, "{}", id(bot, true));
        }

    } else if (auto top = def->isa<Top>()) {
        if (top->sym().empty())
            tab.lnprint(os, "(top {})", emit_type(bb, top->type()));
        else {
            bb.assign(tab, slotted(), id(top), "(top {})", emit_type(bb, top->type()));
            tab.lnprint(os, "{}", id(top, true));
        }

    } else if (def->isa_imm<Rule>()) {
        assert("false" && "TODO no vars in immutable Rule");
    } else if (auto rule = def->isa_mut<Rule>()) {
        toggle_slots();
        auto meta_var_val = emit_var(bb, rule->var(), rule->dom(), true);
        auto lhs_val      = emit_bb(bb, rule->lhs());
        auto rhs_val      = emit_bb(bb, rule->rhs());
        auto guard_val    = emit_bb(bb, rule->guard());
        toggle_slots();
        tab.lnprint(os, "{}", id(rule, true));
        print(decls_, "(rule {} {} {} {} {})\n\n", indent(1, id(rule)), indent(1, meta_var_val), indent(1, lhs_val),
              indent(1, rhs_val), indent(1, guard_val));

    } else if (auto inj = def->isa<Inj>()) {
        print(os, "{}", emit_node(bb, inj, "inj", false, true));

    } else if (auto merge = def->isa<Merge>()) {
        print(os, "{}", emit_node(bb, merge, "merge", true, true));

    } else if (auto match = def->isa<Match>()) {
        print(os, "{}", emit_node(bb, match, "match", true));

    } else if (auto proxy = def->isa<Proxy>()) {
        auto type_val = emit_bb(bb, proxy->type());
        auto pass_val = proxy->pass();
        auto tag_val  = proxy->tag();
        std::vector<std::string> op_vals;
        for (auto op : proxy->ops())
            if (auto op_val = emit_bb(bb, op); !op_val.empty()) op_vals.push_back(op_val);

        if (proxy->sym().empty()) {
            tab.lnprint(os, "(proxy");
            print(os, "{}", type_val);
            // pass_val and tag_val are not emitted via emit_bb and therefore have no
            // leading newlines and indentation levels so we add those here
            ++tab;
            tab.lnprint(os, "{}", pass_val);
            tab.lnprint(os, "{}", tag_val);
            --tab;
            // TODO: variadic ops as cons for slotted
            for (auto op_val : op_vals)
                print(os, "{}", op_val);
            print(os, ")");
        } else {
            bb.assign(tab, slotted(), id(proxy), [&](Tab tab, auto& os) {
                ++tab;
                tab.lnprint(os, "(proxy");
                ++tab;
                print(os, "{}", type_val);
                ++tab;
                tab.lnprint(os, "{}", pass_val);
                tab.lnprint(os, "{}", tag_val);
                --tab;
                // TODO: variadic ops as cons for slotted
                for (auto op_val : op_vals)
                    print(os, "{}", indent(tab.indent(), op_val));
                --tab;
                print(os, ")");
                --tab;
            });
            tab.lnprint(os, "{}", id(proxy, true));
        }

    } else {
        error("Unhandled Def in SExpr backend: {} : {}", def, def->type());
        fe::unreachable();
    }
    --tab;

    return os.str();
}

void emit(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream);
    emitter.run();
}

void emit_slotted(World& world, std::ostream& ostream) {
    Emitter emitter(world, ostream, true);
    emitter.run();
}

} // namespace mim::sexpr
