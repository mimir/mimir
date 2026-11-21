#include "mim/plug/x86_isel/gen.h"

#include <cctype>

#include <map>
#include <print>
#include <set>

#include <mim/driver.h>
#include <mim/plugin.h>
#include <mim/rule.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>
#include <mim/plug/x86/autogen.h>
#include <mim/plug/x86/rule_table.h>

#include "mim/plug/x86_isel/x86_isel.h"

namespace mim::plug::x86_isel {

using x86::rules::Want;

namespace {

enum class Kind : u8 { Hole, Axm, Lit, Nat, Idx, App, Tuple, Pack, Extract, Sigma, Arr };

constexpr std::string_view name(Want want) {
    switch (want) {
#define X(w) \
    case Want::w: return #w;
        MIM_X86_RULE_WANTS(X)
#undef X
    }
    std::unreachable();
}

/// Does @p def use the meta variable @p i of @p var, by its projection or by @p var as a whole?
bool uses(const Def* def, const Var* var, size_t i, DefSet& done) {
    if (def == var) return true;
    if (!def || !done.emplace(def).second) return false;
    if (auto ex = def->isa<Extract>(); ex && ex->tuple() == var)
        if (auto j = Lit::isa(ex->index())) return *j == i;
    if (auto type = def->type(); type && uses(type, var, i, done)) return true;
    for (auto op : def->ops())
        if (uses(op, var, i, done)) return true;
    return false;
}

bool uses(const Def* def, const Var* var, size_t i) {
    auto done = DefSet();
    return uses(def, var, i, done);
}

/// The number of x86 instructions in @p def; an address is none.
size_t num_insns(const Def* def, DefSet& done) {
    if (def->isa_mut() || !done.emplace(def).second) return 0;
    size_t n = 0;
    if (auto [axm, curry, _] = Axm::get(def);
        axm && curry == 0 && Annex::flags2plugin(axm->flags()) == x86::Plugin_Id && !Axm::isa<x86::Addr>(def->type()))
        ++n;
    for (auto op : def->ops())
        n += num_insns(op, done);
    return n;
}

class Table {
public:
    void collect(const Def* def) {
        if (auto rule = def->isa<Rule>())
            add(rule);
        else if (auto tuple = def->isa<Tuple>())
            for (auto op : tuple->ops())
                collect(op);
    }

    void dump(std::ostream& os) const;

private:
    struct Node {
        Kind kind;
        u64 val = 0;
        std::vector<u32> ops;
        auto operator<=>(const Node&) const = default;
    };

    struct Meta {
        Want want;
        u32 type;
    };

    struct Rewrite {
        std::string name;
        std::string type;
        size_t cost;
        flags_t root, head;
        u32 lhs, rhs;
        std::vector<Meta> metas;
    };

    void add(const Rule*);
    std::vector<Meta> metas(const Rule*);
    void guard(const Def*, std::vector<Meta>&);
    u32 node(const Def*);
    u32 node_(const Def*);
    u32 make(Node, std::string comment = {});
    u32 hole(u64 i) { return make({Kind::Hole, i, {}}); }

    DefSet seen_;
    const Var* var_   = nullptr;
    size_t num_metas_ = 0;
    DefMap<u32> def2node_;
    std::map<Node, u32> node2id_;
    std::vector<Node> nodes_;
    std::vector<std::string> comments_;
    std::vector<Rewrite> rewrites_;
    friend class Code;
};

void Table::add(const Rule* rule) {
    if (!seen_.emplace(rule).second) return;
    if (!rule->is_closed()) rule->blame("x86_isel: rule `{}` is not closed", rule).bail();
    if (rule->guard() == rule->world().lit_ff()) return;

    var_               = rule->has_var();
    num_metas_         = var_ ? var_->num_projs() : 0;
    auto [root, _, __] = Axm::get(rule->lhs());
    if (!root) rule->blame("x86_isel: the lhs of `{}` must apply an axiom", rule).bail();
    auto head = std::get<0>(Axm::get(rule->rhs()));

    auto done = DefSet();
    rewrites_.emplace_back(Rewrite{.name  = rule->sym().str(),
                                   .type  = std::format("{} ↝ {}", rule->dom(), rule->codom()),
                                   .cost  = num_insns(rule->rhs(), done),
                                   .root  = root->flags(),
                                   .head  = head ? head->flags() : 0,
                                   .lhs   = node(rule->lhs()),
                                   .rhs   = node(rule->rhs()),
                                   .metas = metas(rule)});
    var_ = nullptr;
}

std::vector<Table::Meta> Table::metas(const Rule* rule) {
    auto res = std::vector<Meta>();
    auto n   = num_metas_;
    for (size_t i = 0; i != n; ++i) {
        auto meta  = var_->proj(n, i);
        auto type  = meta->type();
        bool param = !uses(rule->rhs(), var_, i);
        for (size_t j = 0; j != n && !param; ++j)
            param = j != i && uses(var_->proj(n, j)->type(), var_, i);

        auto want = Want::Param;
        if (!param) {
            if (Idx::isa(type)) {
                want = Want::Reg;
            } else if (auto addr = Axm::isa<x86::Addr>(type)) {
                auto i = Lit::isa(addr->arg());
                want   = !i ? Want::Addr : *i ? Want::Index : Want::Base;
            } else {
                rule->blame("x86_isel: meta variable `{}` of `{}` has type `{}`, which is no operand", meta, rule, type)
                    .bail();
            }
        }
        res.emplace_back(want, node(type));
    }
    guard(rule->guard(), res);
    return res;
}

void Table::guard(const Def* g, std::vector<Meta>& metas) {
    auto& w = g->world();
    if (g == w.lit_tt()) return;
    if (auto conj = Axm::isa(core::bit2::and_, g)) {
        auto [a, b] = conj->args<2>();
        guard(a, metas);
        guard(b, metas);
        return;
    }

    auto set = [&](const Def* meta, Want want) {
        for (size_t i = 0; i != num_metas_; ++i)
            if (var_->proj(num_metas_, i) == meta && metas[i].want == Want::Reg) {
                metas[i].want = want;
                return;
            }
        g->blame("x86_isel: guard `{}` must constrain an operand of type `Idx s`", g).bail();
    };

    if (auto imm = Axm::isa<x86_isel::imm>(g))
        set(imm->arg(), Want::Imm);
    else if (auto sh = Axm::isa<x86_isel::sh>(g))
        set(sh->arg(), Want::Sh);
    else
        g->blame("x86_isel: guard `{}` is no conjunction of `x86_isel.imm` and `x86_isel.sh`", g).bail();
}

u32 Table::node(const Def* def) {
    if (auto id = fe::lookup(def2node_, def)) return *id;
    auto id               = node_(def);
    return def2node_[def] = id;
}

u32 Table::node_(const Def* def) {
    if (var_ && def == var_) {
        if (num_metas_ == 1) return hole(0);
        auto ops = std::vector<u32>();
        for (size_t i = 0; i != num_metas_; ++i)
            ops.emplace_back(hole(i));
        return make({Kind::Tuple, 0, std::move(ops)});
    }
    if (auto ex = def->isa<Extract>(); ex && var_ && ex->tuple() == var_)
        if (auto i = Lit::isa(ex->index())) return hole(*i);

    if (auto axm = def->isa<Axm>()) return make({Kind::Axm, axm->flags(), {}}, axm->sym().str());
    if (auto lit = def->isa<Lit>()) return make({Kind::Lit, lit->get(), {node(lit->type())}});
    if (def->isa<Nat>()) return make({Kind::Nat, 0, {}});
    if (def->isa<Idx>()) return make({Kind::Idx, 0, {}});
    if (auto app = def->isa<App>()) return make({Kind::App, 0, {node(app->callee()), node(app->arg())}});
    if (auto ex = def->isa<Extract>()) return make({Kind::Extract, 0, {node(ex->tuple()), node(ex->index())}});
    if (auto pack = def->isa_imm<Pack>()) return make({Kind::Pack, 0, {node(pack->arity()), node(pack->body())}});
    if (auto arr = def->isa_imm<Arr>()) return make({Kind::Arr, 0, {node(arr->arity()), node(arr->body())}});

    auto ops = std::vector<u32>();
    if (def->isa<Tuple>() || def->isa_imm<Sigma>())
        for (auto op : def->ops())
            ops.emplace_back(node(op));
    if (def->isa<Tuple>()) return make({Kind::Tuple, 0, std::move(ops)});
    if (def->isa_imm<Sigma>()) return make({Kind::Sigma, 0, std::move(ops)});

    def->blame("x86_isel: cannot put `{}` into a rule table", def).bail();
}

u32 Table::make(Node node, std::string comment) {
    if (auto id = fe::lookup(node2id_, node)) return *id;
    auto id = u32(nodes_.size());
    node2id_.emplace(node, id);
    nodes_.emplace_back(std::move(node));
    comments_.emplace_back(std::move(comment));
    return id;
}

/// Emits C++ that matches and builds the terms of the node pool of a Table.
class Code {
public:
    Code(std::ostream& os, const Table& table)
        : os_(os)
        , t_(table) {}

    void match(size_t id, u32 lhs, const std::vector<u32>& types);
    void build(u32 rhs);

private:
    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::println(os_, "    {}", std::format(fmt, std::forward<Args>(args)...));
    }

    std::string fresh(char prefix) { return std::format("{}{}", prefix, next_++); }
    const Table::Node& node(u32 n) const { return t_.nodes_[n]; }
    const std::string& comment(u32 n) const { return t_.comments_[n]; }
    /// Binds @p expr to a fresh variable, unless it is one.
    std::string name(const std::string& expr);
    void match(u32 n, const std::string& expr);
    void match2(u32 n, const std::string& expr, std::string_view isa, std::string_view op0, std::string_view op1);
    std::string build_(u32 n);

    std::ostream& os_;
    const Table& t_;
    size_t next_ = 0;
    std::vector<bool> bound_;
    const std::vector<u32>* types_ = nullptr;
    std::map<u32, std::string> built_;
};

void Code::match(size_t id, u32 lhs, const std::vector<u32>& types) {
    next_  = 0;
    types_ = &types;
    bound_.assign(types.size(), false);
    std::println(os_, "inline bool match_{}(const Def* def, bool swap, [[maybe_unused]] const Def** m) {{", id);

    auto& n = node(lhs);
    if (n.kind == Kind::App && node(n.ops[1]).kind == Kind::Tuple && node(n.ops[1]).ops.size() == 2) {
        auto app = fresh('d');
        line("auto {} = def->isa<App>();", app);
        line("if (!{}) return false;", app);
        match(n.ops[0], app + "->callee()");
        auto arg = name(app + "->arg()");
        line("if (Lit::isa({}->arity()) != 2) return false;", arg);
        match(node(n.ops[1]).ops[0], std::format("{}->proj(2, swap ? 1 : 0)", arg));
        match(node(n.ops[1]).ops[1], std::format("{}->proj(2, swap ? 0 : 1)", arg));
    } else {
        line("if (swap) return false;");
        match(lhs, "def");
    }
    line("return true;");
    std::println(os_, "}}");
    std::println(os_);
}

std::string Code::name(const std::string& expr) {
    if (std::ranges::all_of(expr, [](char c) { return std::isalnum(c) || c == '_'; })) return expr;
    auto res = fresh('d');
    line("auto {} = {};", res, expr);
    return res;
}

void Code::match2(u32 n, const std::string& e, std::string_view isa, std::string_view op0, std::string_view op1) {
    auto x = fresh('d');
    line("auto {} = {}->{};", x, e, isa);
    line("if (!{}) return false;", x);
    match(node(n).ops[0], std::format("{}->{}()", x, op0));
    match(node(n).ops[1], std::format("{}->{}()", x, op1));
}

void Code::match(u32 n, const std::string& e) {
    const auto& x = node(n);
    switch (x.kind) {
        case Kind::Hole: {
            auto i = x.val;
            if (bound_[i]) {
                line("if (m[{}] != {}) return false;", i, e);
                return;
            }
            bound_[i] = true;
            line("m[{}] = {};", i, e);
            return match((*types_)[i], std::format("m[{}]->type()", i));
        }
        case Kind::Axm: {
            auto d = name(e);
            line("if (!{0}->isa<Axm>() || {0}->flags() != {1:#x}) return false; // {2}", d, x.val, comment(n));
            return;
        }
        case Kind::Lit: {
            auto lit = fresh('d');
            line("auto {} = {}->isa<Lit>();", lit, e);
            line("if (!{0} || {0}->get() != {1:#x}) return false;", lit, x.val);
            return match(x.ops[0], lit + "->type()");
        }
        case Kind::Nat: line("if (!{}->isa<Nat>()) return false;", e); return;
        case Kind::Idx: line("if (!{}->isa<Idx>()) return false;", e); return;
        case Kind::App: return match2(n, e, "isa<App>()", "callee", "arg");
        case Kind::Extract: return match2(n, e, "isa<Extract>()", "tuple", "index");
        case Kind::Pack: return match2(n, e, "isa_imm<Pack>()", "arity", "body");
        case Kind::Arr: return match2(n, e, "isa_imm<Arr>()", "arity", "body");
        case Kind::Sigma: {
            auto sigma = fresh('d');
            line("auto {} = {}->isa_imm<Sigma>();", sigma, e);
            line("if (!{0} || {0}->num_ops() != {1}) return false;", sigma, x.ops.size());
            for (size_t j = 0; j != x.ops.size(); ++j)
                match(x.ops[j], std::format("{}->op({})", sigma, j));
            return;
        }
        case Kind::Tuple: {
            // η: any term of the tuple's arity matches by its projections.
            auto d = name(e);
            line("if (Lit::isa({}->arity()) != {}) return false;", d, x.ops.size());
            for (size_t j = 0; j != x.ops.size(); ++j)
                match(x.ops[j], std::format("{}->proj({}, {})", d, x.ops.size(), j));
            return;
        }
    }
    std::unreachable();
}

void Code::build(u32 rhs) {
    next_ = 0;
    built_.clear();
    std::println(os_, "inline const Def* build_{}(World& w, [[maybe_unused]] const Def* const* h) {{", rhs);
    line("return {};", build_(rhs));
    std::println(os_, "}}");
    std::println(os_);
}

std::string Code::build_(u32 n) {
    if (auto i = built_.find(n); i != built_.end()) return i->second;
    const auto& x = node(n);
    if (x.kind == Kind::Hole) return built_[n] = std::format("h[{}]", x.val);

    auto ops = std::vector<std::string>();
    for (auto op : x.ops)
        ops.emplace_back(build_(op));
    auto res = fresh('b');
    switch (x.kind) {
        case Kind::Axm: line("auto {} = w.annex({:#x}); // {}", res, x.val, comment(n)); break;
        case Kind::Lit: line("auto {} = w.lit({}, {:#x});", res, ops[0], x.val); break;
        case Kind::Nat: line("auto {} = w.type_nat();", res); break;
        case Kind::Idx: line("auto {} = w.type_idx();", res); break;
        case Kind::App: line("auto {} = w.app({}, {});", res, ops[0], ops[1]); break;
        case Kind::Extract: line("auto {} = w.extract({}, {});", res, ops[0], ops[1]); break;
        case Kind::Pack: line("auto {} = w.pack({}, {});", res, ops[0], ops[1]); break;
        case Kind::Arr: line("auto {} = w.arr({}, {});", res, ops[0], ops[1]); break;
        case Kind::Tuple: line("auto {} = w.tuple({{{}}});", res, fe::Join(ops, ", ")); break;
        case Kind::Sigma: line("auto {} = w.sigma({{{}}});", res, fe::Join(ops, ", ")); break;
        case Kind::Hole: std::unreachable();
    }
    return built_[n] = res;
}

void Table::dump(std::ostream& os) const {
    std::println(os, "// Generated by `x86_isel.gen`; do not edit.");
    std::println(os, "#pragma once");
    std::println(os);
    std::println(os, "#include <mim/world.h>");
    std::println(os);
    std::println(os, "#include \"mim/plug/x86/rule_table.h\"");
    std::println(os);
    std::println(os, "namespace mim::plug::x86::rules {{");
    std::println(os);

    auto code     = Code(os, *this);
    auto matchers = std::map<std::pair<u32, std::vector<u32>>, size_t>();
    auto ids      = std::vector<size_t>();
    for (const auto& r : rewrites_) {
        auto types = std::vector<u32>();
        for (auto m : r.metas)
            types.emplace_back(m.type);
        auto [i, fresh] = matchers.emplace(std::pair(r.lhs, types), matchers.size());
        if (fresh) code.match(i->second, r.lhs, types);
        ids.emplace_back(i->second);
    }
    auto built = std::set<u32>();
    for (const auto& r : rewrites_)
        if (built.emplace(r.rhs).second) code.build(r.rhs);

    std::println(os, "// clang-format off");
    std::println(os, "inline constexpr Want wants[] = {{");
    size_t num_wants = 0;
    for (const auto& r : rewrites_) {
        if (r.metas.empty()) continue;
        std::print(os, "   ");
        for (auto m : r.metas)
            std::print(os, " Want::{},", name(m.want));
        std::println(os, " // {}: {}", r.name, r.type);
        num_wants += r.metas.size();
    }
    // An empty array is ill-formed.
    if (num_wants == 0) std::println(os, "    Want::Param,");
    std::println(os, "}};");
    std::println(os);

    std::println(os, "inline constexpr Rewrite rewrites[] = {{");
    size_t first = 0;
    for (size_t i = 0, e = rewrites_.size(); i != e; ++i) {
        const auto& r = rewrites_[i];
        std::println(os, "    {{\"{}\", {}, {:#x}, {:#x}, &match_{}, &build_{}, {}, {}}}, // {}", r.name, r.cost,
                     r.root, r.head, ids[i], r.rhs, first, r.metas.size(), r.type);
        first += r.metas.size();
    }
    std::println(os, "}};");
    std::println(os, "// clang-format on");
    std::println(os);
    std::println(os, "}} // namespace mim::plug::x86::rules");
}

} // namespace

void Gen::start() {
    auto& lib  = *driver().library();
    auto flags = std::vector<flags_t>();
    for (const auto& [f, _] : lib.annexes())
        if (Annex::flags2plugin(f) == Plugin_Id) flags.emplace_back(f);
    std::ranges::sort(flags);

    auto table = Table();
    for (auto f : flags)
        table.collect(world().annex(f));

    auto out = Out(std::string(arg_value(args(), "o", "output").value_or("-")));
    table.dump(*out.os());
}

} // namespace mim::plug::x86_isel
