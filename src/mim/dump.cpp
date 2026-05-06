#include <fstream>
#include <ostream>

#include <fe/assert.h>

#include "mim/driver.h"
#include "mim/nest.h"

#include "mim/ast/tok.h"
#include "mim/util/util.h"

using namespace std::literals;

// During dumping, we classify Defs according to the following logic:
// * Inline: These Defs are *always* displayed with all of its operands "inline".
//   E.g.: (1, 2, 3).
// * All other Defs are referenced by its name/unique_name (see id) when they appear as an operand.
// * Mutables are either classifed as "decl" (see isa_decl).
//   In this case, recursing through the Defs' operands stops and this particular Decl is dumped as its own thing.
// * Or - if they are not a "decl" - they are basicallally handled like immutables.

namespace mim {

namespace {

Def* isa_decl(const Def* def) {
    if (auto mut = def->isa_mut()) {
        if (mut->is_external() || mut->isa<Lam>() || (mut->sym() && mut->sym() != '_')) return mut;
    }
    return nullptr;
}

std::string id(const Def* def) {
    if (def->is_external() || (!def->is_set() && def->isa<Lam>())) return def->sym().str();
    return def->unique_name();
}

std::string_view external(const Def* def) {
    if (def->is_external()) return "extern "sv;
    return ""sv;
}

using ast::Assoc;
using ast::Prec;
using ast::prec_assoc;

Prec def2prec(const Def* def) {
    if (def->isa<Extract>()) return Prec::Extract;
    if (auto pi = def->isa<Pi>(); pi && !Pi::isa_cn(pi)) return Prec::Arrow;
    if (auto app = def->isa<App>()) {
        if (auto size = Idx::isa(app)) {
            if (auto l = Lit::isa(size)) {
                // clang-format off
                switch (*l) {
                    case 0x0'0000'0002_n:
                    case 0x0'0000'0100_n:
                    case 0x0'0001'0000_n:
                    case 0x1'0000'0000_n:
                    case             0_n: return Prec::Lit;
                    default: break;
                }
                // clang-format on
            }
        }
        return Prec::App;
    }

    return Prec::Lit;
}

/// This is a wrapper to dump a Def.
class Op {
public:
    Op(const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : def_(def)
        , prec_(prec)
        , is_left_(is_left) {}
    static Op l(const Def* def, Prec prec = Prec::Bot) { return {def, prec, true}; }
    static Op r(const Def* def, Prec prec = Prec::Bot) { return {def, prec, false}; }

    /// @name Getters
    ///@{
    Prec prec() const { return prec_; }
    bool is_left() const { return is_left_; }
    const Def* def() const { return def_; }
    const Def* operator->() const { return def_; }
    const Def* operator*() const { return def_; }
    explicit operator bool() const { return def_ != nullptr; }
    ///@}

private:
    const Def* def_;
    Prec prec_;
    bool is_left_;

    /// This will stream @p def as an operand.
    /// This is usually `id(def)` unless it can be displayed Inline.
    friend std::ostream& operator<<(std::ostream&, Op);
};

/// This is a wrapper to dump a Def "inline" and print it with all of its operands.
class Dump : public Op {
public:
    Dump(const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : Op(def, prec, is_left) {}
    Dump(Op op)
        : Dump(op.def(), op.prec(), op.is_left()) {}

    explicit operator bool() const { return is_inline(); }

    bool is_inline() const {
        if (auto mut = def()->isa_mut()) {
            if (isa_decl(mut)) return false;
            return true;
        }

        if (def()->is_closed()) return true;

        if (auto app = def()->isa<App>()) {
            if (app->type()->isa<Pi>()) return true; // curried apps are printed inline
            if (app->type()->isa<Type>()) return true;
            if (app->callee()->isa<Axm>()) return app->callee_type()->num_doms() <= 1;
            return false;
        }

        return true;
    }

    bool needs_parens() const {
        if (!is_inline()) return false;

        auto child_prec = def2prec(def());
        if (child_prec < prec()) return true;
        if (child_prec > prec()) return false;

        switch (prec_assoc(prec())) {
            case Assoc::R: return is_left();
            case Assoc::L: return !is_left();
            case Assoc::N: return false;
        }
        fe::unreachable();
    }

    friend std::ostream& operator<<(std::ostream&, Dump);
};

auto ptrn(const Def* def, const Def* type) {
    return StreamFn{[=](std::ostream& os) -> std::ostream& {
        if (!def) return os << Op(type);

        auto projs = def->tprojs();
        if (projs.size() == 1 || std::ranges::all_of(projs, [](auto def) { return !def; }))
            return print(os, "{}: {}", def->unique_name(), Op(type));

        size_t i  = 0;
        auto elem = Elem(projs, [&](auto proj) { os << ptrn(proj, type->proj(i++)); });
        return print(os, "({, }) as {}", elem, def->unique_name());
    }};
}

auto bndr(const Def* def, const Def* type) {
    return StreamFn{[=](std::ostream& os) -> std::ostream& {
        if (def) return os << ptrn(def, type);
        return print(os, "_: {}", Op(type));
    }};
}

auto curry(const Def* def, const Def* type, bool implicit, bool paren_style, size_t limit, bool alias) {
    return StreamFn{[=](std::ostream& os) -> std::ostream& {
        auto l = implicit ? '{' : paren_style ? '(' : '[';
        auto r = implicit ? '}' : paren_style ? ')' : ']';

        if (limit == 0) return os << l << r;
        if (limit == 1) return print(os, "{}{}{}", l, bndr(def ? def->tproj(0) : nullptr, type->tproj(0)), r);

        auto elem = [&](auto i) { os << bndr(def ? def->tproj(i) : nullptr, type->tproj(i)); };
        print(os, "{}{, }{}", l, Elem(std::views::iota(size_t(0), limit), elem), r);
        if (alias && def) print(os, " as {}", def->unique_name());
        return os;
    }};
}

std::ostream& operator<<(std::ostream& os, Op op) {
    if (*op == nullptr) return os << "<nullptr>";
    if (auto d = Dump(op)) return os << d;
    return os << id(*op);
}

std::ostream& operator<<(std::ostream& os, Dump d) {
    if (auto mut = d->isa_mut(); mut && !mut->is_set()) return os << "unset";
    if (d.needs_parens()) return print(os, "({})", Dump(*d));

    bool ascii = d->world().flags().ascii;
    auto arw   = ascii ? "->" : "→";
    auto al    = ascii ? "<<" : "«";
    auto ar    = ascii ? ">>" : "»";
    auto pl    = ascii ? "(<" : "‹";
    auto pr    = ascii ? ">)" : "›";
    auto bot   = ascii ? "bot" : "⊥";
    auto top   = ascii ? "top" : "⊤";

    if (auto type = d->isa<Type>()) {
        if (auto level = Lit::isa(type->level()); level && !ascii) {
            if (level == 0) return print(os, "*");
            if (level == 1) return print(os, "□");
        }
        return print(os, "Type {}", Op::r(type->level(), Prec::App));
    } else if (d->isa<Nat>()) {
        return print(os, "Nat");
    } else if (d->isa<Idx>()) {
        return print(os, "Idx");
    } else if (auto ext = d->isa<Ext>()) {
        return print(os, "{}:{}", ext->isa<Bot>() ? bot : top, Op::r(ext->type(), Prec::Lit));
    } else if (auto axm = d->isa<Axm>()) {
        const auto name = axm->sym();
        return print(os, "{}{}", name[0] == '%' ? "" : "%", name);
    } else if (auto lit = d->isa<Lit>()) {
        if (lit->type()->isa<Nat>()) {
            // clang-format off
            switch (lit->get()) {
                case 0x0'0000'0100_n: return os << "i8";
                case 0x0'0001'0000_n: return os << "i16";
                case 0x1'0000'0000_n: return os << "i32";
                default: return print(os, "{}", lit->get());
            }
            // clang-format on
        } else if (auto size = Idx::isa(lit->type())) {
            if (auto s = Lit::isa(size)) {
                // clang-format off
                switch (*s) {
                    case 0x0'0000'0002_n: return os << (lit->get<bool>() ? "tt" : "ff");
                    case 0x0'0000'0100_n: return os << lit->get() << "I8";
                    case 0x0'0001'0000_n: return os << lit->get() << "I16";
                    case 0x1'0000'0000_n: return os << lit->get() << "I32";
                    case             0_n: return os << lit->get() << "I64";
                    default: {
                        os << lit->get();
                        std::vector<uint8_t> digits;
                        for (auto z = *s; z; z /= 10) digits.emplace_back(z % 10);

                        if (ascii) {
                            os << '_';
                            for (auto d : digits | std::ranges::views::reverse)
                                os << char('0' + d);
                        } else {
                            for (auto d : digits | std::ranges::views::reverse)
                                os << uint8_t(0xE2) << uint8_t(0x82) << (uint8_t(0x80 + d));
                        }
                        return os;
                    }
                }
                // clang-format on
            }
        }
        return print(os, "{}:{}", lit->get(), Op::r(lit->type(), Prec::Lit));
    } else if (auto ex = d->isa<Extract>()) {
        if (ex->tuple()->isa<Var>() && ex->index()->isa<Lit>()) return os << ex->unique_name();
        return print(os, "{}#{}", Op::l(ex->tuple(), Prec::Extract), Op::r(ex->index(), Prec::Extract));
    } else if (auto var = d->isa<Var>()) {
        return os << var->unique_name();
    } else if (auto [pi, var] = d->isa_binder<Pi>(); pi) {
        auto l = pi->is_implicit() ? '{' : '[';
        auto r = pi->is_implicit() ? '}' : ']';
        return print(os, "{}{}: {}{} {} {}", l, Op(var), Op(pi->dom()), r, arw, Op::r(pi->dom(), Prec::Arrow));
    } else if (auto pi = d->isa<Pi>()) {
        if (Pi::isa_cn(pi)) return print(os, "Cn {}", Op(pi->dom()));
        return print(os, "{} {} {}", Op::l(pi->dom(), Prec::Arrow), arw, Op::r(pi->dom(), Prec::Arrow));
    } else if (auto lam = d->isa<Lam>()) {
        // TODO this output is really confuinsg
        return print(os, "{}, {}", Op(lam->filter()), Op(lam->body()));
    } else if (auto app = d->isa<App>()) {
        if (auto size = Idx::isa(app)) {
            if (auto l = Lit::isa(size)) {
                // clang-format off
                switch (*l) {
                    case 0x0'0000'0002_n: return os << "Bool";
                    case 0x0'0000'0100_n: return os << "I8";
                    case 0x0'0001'0000_n: return os << "I16";
                    case 0x1'0000'0000_n: return os << "I32";
                    case             0_n: return os << "I64";
                    default: break;
                }
                // clang-format on
            }
        }

        return print(os, "{} {}", Op::l(app->callee(), Prec::App), Op::r(app->arg(), Prec::App));
    } else if (auto [sigma, var] = d->isa_binder<Sigma>(); sigma) {
        size_t i  = 0;
        auto elem = Elem(sigma->ops(), [&os, sigma, &i](auto op) {
            if (auto v = sigma->var(i++))
                print(os, "{}: {}", v, Op(op));
            else
                os << Op(op);
        });

        return print(os, "[{, }]", elem);
    } else if (auto sigma = d->isa<Sigma>()) {
        return print(os, "[{, }]", elems<Op>(os, sigma->ops()));
    } else if (auto tuple = d->isa<Tuple>()) {
        return print(os, "({, })", elems<Op>(os, tuple->ops()));
    } else if (auto [arr, var] = d->isa_binder<Arr>(); arr) {
        return print(os, "{}{}: {}; {}{}", al, var, Op(arr->arity()), Op(arr->body()), ar);
    } else if (auto arr = d->isa<Arr>()) {
        return print(os, "{}{}; {}{}", al, Op(arr->arity()), Op(arr->body()), ar);
    } else if (auto [pack, var] = d->isa_binder<Pack>(); pack) {
        return print(os, "{}{}: {}; {}{}", pl, pack->var(), Op(pack->arity()), Op(pack->body()), pr);
    } else if (auto pack = d->isa<Pack>()) {
        return print(os, "{}{}; {}{}", pl, Op(pack->arity()), Op(pack->body()), pr);
    } else if (auto proxy = d->isa<Proxy>()) {
        return print(os, ".proxy#{}#{} {, }", proxy->pass(), proxy->tag(), elems<Op>(os, proxy->ops()));
    } else if (auto bound = d->isa<Bound>()) {
        auto op = bound->isa<Join>() ? "∪" : "∩"; // TODO ascii
        if (auto mut = d->isa_mut()) print(os, "{}{}: {}", op, mut->unique_name(), Op(mut->type()));
        return print(os, "{}({, })", op, elems<Op>(os, bound->ops()));
    }

    // other
    if (d->flags() == 0) return print(os, "({} {, })", d->node_name(), d->ops());
    return print(os, "({}#{} {, })", d->node_name(), d->flags(), elems<Op>(os, d->ops()));
}

/*
 * Dumper
 */

/// This thing operates in two modes:
/// 1. The output of decls is driven by the Nest.
/// 2. Alternatively, decls are output as soon as they appear somewhere during recurse%ing.
///     Then, they are pushed to Dumper::muts.
class Dumper {
public:
    Dumper(std::ostream& os, const Nest* nest = nullptr)
        : os(os)
        , nest(nest) {}

    void dump(Def*);
    void dump_lam(Lam*);
    void dump_let(const Def*);
    void recurse(const Nest::Node*);
    void recurse(const Def*, bool first = false);

    std::ostream& os;
    const Nest* nest;
    Tab tab;
    unique_queue<MutSet> muts;
    DefSet defs;
};

void Dumper::dump(Def* mut) {
    if (auto lam = mut->isa<Lam>()) {
        dump_lam(lam);
        return;
    }

    auto mut_prefix = [&](const Def* def) {
        if (def->isa<Sigma>()) return "Sigma";
        if (def->isa<Arr>()) return "Arr";
        if (def->isa<Pack>()) return "pack";
        if (def->isa<Pi>()) return "Pi";
        if (def->isa<Hole>()) return "Hole";
        if (def->isa<Rule>()) return "Rule";
        fe::unreachable();
    };

    auto mut_op0 = [&](const Def* def) -> std::ostream& {
        if (auto sig = def->isa<Sigma>()) return print(os, ", {}", sig->num_ops());
        if (auto arr = def->isa<Arr>()) return print(os, ", {}", arr->arity());
        if (auto pack = def->isa<Pack>()) return print(os, ", {}", pack->arity());
        if (auto pi = def->isa<Pi>()) return print(os, ", {}", pi->dom());
        if (auto hole = def->isa_mut<Hole>()) return hole->is_set() ? print(os, ", {}", hole->op()) : print(os, ", ??");
        if (auto rule = def->isa<Rule>()) return print(os, "{} => {}", rule->lhs(), rule->rhs());
        fe::unreachable();
    };

    if (!mut->is_set()) {
        tab.print(os, "{}: {} = {{ <unset> }};", id(mut), mut->type());
        return;
    }

    tab.print(os, "{} {}{}: {}", mut_prefix(mut), external(mut), id(mut), mut->type());
    mut_op0(mut);
    if (mut->var()) { // TODO rewrite with dedicated methods
        if (auto e = mut->num_vars(); e != 1) {
            print(os, "{, }", Elem(mut->vars(), [&](auto def) {
                      if (def)
                          os << def->unique_name();
                      else
                          os << "<TODO>";
                  }));
        } else {
            print(os, ", @{}", mut->var()->unique_name());
        }
    }
    tab.println(os, " = {{");
    ++tab;
    if (nest) recurse((*nest)[mut]);
    recurse(mut);
    tab.print(os, "{, }\n", mut->ops());
    --tab;
    tab.print(os, "}};\n");
}

void Dumper::dump_lam(Lam* lam) {
    std::vector<Lam*> currys;
    for (Lam* curr = lam;;) {
        currys.emplace_back(curr);
        if (auto body = curr->body())
            if (auto next = body->isa_mut<Lam>()) {
                curr = next;
                continue;
            }
        break;
    }

    auto last   = currys.back();
    auto is_fun = Lam::isa_returning(last);
    auto is_con = Lam::isa_cn(last) && !is_fun;

    tab.print(os, "{} {}{}", is_fun ? "fun" : is_con ? "con" : "lam", external(lam), id(lam));
    for (auto* c : currys) {
        os << ' ';
        auto num_doms = c->var() ? c->var()->num_tprojs() : c->type()->dom()->num_tprojs();
        auto limit    = is_fun && c == last ? num_doms - 1 : num_doms;
        os << curry(c->var(), c->type()->dom(), c->type()->is_implicit(), !is_con, limit, !is_fun || c != last);
        if (is_con && c == last) print(os, "@({})", c->filter());
    }

    if (is_fun)
        print(os, ": {} =", last->ret_dom());
    else if (!is_con)
        print(os, ": {} =", last->type()->codom());
    else
        print(os, " =");
    os << '\n';

    ++tab;
    if (last->is_set()) {
        if (nest && currys.size() == 1) recurse((*nest)[lam]);
        for (auto* curry : currys)
            recurse(curry->filter());
        recurse(last->body(), true);
        if (last->body()->isa_mut())
            tab.print(os, "{};\n", last->body());
        else
            tab.print(os, "{};\n", Dump(last->body()));
    } else {
        tab.print(os, "<unset>;\n");
    }
    --tab;
    tab.print(os, "\n");
}

void Dumper::dump_let(const Def* def) {
    tab.print(os, "let {}: {} = {};\n", def->unique_name(), Op(def->type()), Dump(def));
}

void Dumper::recurse(const Nest::Node* node) {
    for (auto child : node->children().muts())
        if (auto mut = isa_decl(child)) dump(mut);
}

void Dumper::recurse(const Def* def, bool first /*= false*/) {
    if (auto mut = isa_decl(def)) {
        if (!nest) muts.push(mut);
        return;
    }

    if (!defs.emplace(def).second) return;

    for (auto op : def->deps())
        recurse(op);

    if (!first && !Dump(def)) dump_let(def);
}

} // namespace

/*
 * Def
 */

/// This will stream @p def as an operand.
/// This is usually `id(def)` unless it can be displayed Inline.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    if (auto d = Dump(def)) {
        auto _ = def->world().freeze();
        return os << d;
    }
    return os << id(def);
}

std::ostream& Def::stream(std::ostream& os, int max) const {
    auto _      = world().freeze();
    auto dumper = Dumper(os);

    if (max == 0) {
        os << this << std::endl;
    } else if (auto mut = isa_decl(this)) {
        dumper.muts.push(mut);
    } else {
        dumper.recurse(this);
        dumper.tab.print(os, "{}\n", Dump(this));
        --max;
    }

    for (; !dumper.muts.empty() && max > 0; --max)
        dumper.dump(dumper.muts.pop());

    return os;
}

void Def::dump() const { std::cout << this << std::endl; }
void Def::dump(int max) const { stream(std::cout, max) << std::endl; }

void Def::write(int max, const char* file) const {
    auto ofs = std::ofstream(file);
    stream(ofs, max);
}

void Def::write(int max) const {
    auto file = id(this) + ".mim"s;
    write(max, file.c_str());
}

/*
 * World
 */

void World::dump(std::ostream& os) {
    auto freezer = World::Freezer(*this);
    auto old_gid = curr_gid();

    if (flags().dump_recursive) {
        auto dumper = Dumper(os);
        for (auto mut : externals().muts())
            dumper.muts.push(mut);
        while (!dumper.muts.empty())
            dumper.dump(dumper.muts.pop());
    } else {
        auto nest   = Nest(*this);
        auto dumper = Dumper(os, &nest);

        for (const auto& import : driver().imports())
            print(os, "{} {};\n", import.tag == ast::Tok::Tag::K_plugin ? "plugin" : "import", import.sym);
        dumper.recurse(nest.root());
    }

    assertf(old_gid == curr_gid(), "new nodes created during dump. old_gid: {}; curr_gid: {}", old_gid, curr_gid());
}

void World::dump() { dump(std::cout); }

void World::debug_dump() {
    if (log().level() >= Log::Level::Debug) dump(log().ostream());
}

void World::write(const char* file) {
    auto ofs = std::ofstream(file);
    dump(ofs);
}

void World::write() {
    auto file = name().str() + ".mim"s;
    write(file.c_str());
}

} // namespace mim
