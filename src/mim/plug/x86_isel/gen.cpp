#include "mim/plug/x86_isel/gen.h"

#include <cctype>

#include <print>
#include <ranges>
#include <span>

#include <fe/hash.h>

#include <mim/driver.h>
#include <mim/plugin.h>
#include <mim/rewrite.h>
#include <mim/rule.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>
#include <mim/plug/refly/autogen.h>
#include <mim/plug/x86/autogen.h>
#include <mim/plug/x86/insn.h>
#include <mim/plug/x86/rule_table.h>

#include "mim/plug/x86_isel/x86_isel.h"

namespace mim::plug::x86_isel {

using x86::rules::Ref;
using x86::rules::Ty;
using x86::rules::Want;

namespace {

enum class Kind : u8 { Hole, Axm, Lit, Nat, Idx, App, Tuple, Pack, Extract, Sigma, Arr, Pi, Bot };

/// Flags of an App node.
enum : u64 {
    Implicit = 1, ///< Its callee takes an implicit argument.
    Inner    = 2, ///< It saturates an axiom into a term, so in a term position of an lhs it is an inner node.
    Commutes = 4, ///< It applies a commuting axiom to two operands.
};

/// Flag of an Extract node: it is the memory of a memory operation.
constexpr u64 Mem = 1;

constexpr std::string_view name(Ty ty) {
    switch (ty) {
#define X(t) \
    case Ty::t: return #t;
        MIM_X86_RULE_TYS(X)
#undef X
    }
    std::unreachable();
}

/// What an operand of @p type is: `Base`, `Index`, `Addr` for either, `Scaled` for `[I64, I2]`, `FReg`, else `Reg`.
Want classify(const Def* type) {
    if (auto addr = Axm::isa<x86::Addr>(type)) {
        auto i = Lit::isa(addr->arg());
        return !i ? Want::Addr : *i ? Want::Index : Want::Base;
    }
    if (math::isa_f(type)) return Want::FReg;
    auto& w = type->world();
    if (type == w.sigma({w.type_i64(), w.type_i2()})) return Want::Scaled;
    return Want::Reg;
}

/// The non-terminal of a value of @p type in @p rule.
Ty nonterminal(const Rule* rule, const Def* type) {
    switch (classify(type)) {
        case Want::Base: return Ty::Base;
        case Want::Index: return Ty::Index;
        case Want::Scaled: return Ty::Scaled;
        case Want::Reg: return Ty::Reg;
        case Want::FReg: return Ty::FReg;
        default: rule->blame("x86_isel: `{}` must fix its address form in `{}`", type, rule).bail();
    }
}

/// The non-terminal the rhs of @p rule yields; that of a memory operation is its value.
Ty type(const Rule* rule) {
    auto type = rule->rhs()->type();
    if (type->num_projs() == 2 && Axm::isa<core::M>(type->proj(2, 0))) type = type->proj(2, 1);
    return nonterminal(rule, type);
}

constexpr std::string_view name(Want want) {
    switch (want) {
#define X(w) \
    case Want::w: return #w;
        MIM_X86_RULE_WANTS(X)
#undef X
    }
    std::unreachable();
}

/// Does @p def use @p meta, a projection of the Var @p var or a Var of its own, directly or by @p var as a whole?
bool uses(const Def* def, const Def* meta, const Var* var, DefSet& done) {
    if (def == meta) return true;
    if (!def || !done.emplace(def).second) return false;
    if (auto ex = meta->isa<Extract>(); def == var && ex && ex->tuple() == var) return true;
    if (auto ex = def->isa<Extract>(); ex && ex->tuple() == var && Lit::isa(ex->index())) return false;
    if (auto type = def->type(); type && uses(type, meta, var, done)) return true;
    for (auto op : def->ops())
        if (uses(op, meta, var, done)) return true;
    return false;
}

bool uses(const Def* def, const Def* meta, const Var* var) {
    auto done = DefSet();
    return uses(def, meta, var, done);
}

/// The conditions of the `refly.check`s in @p def, e.g. the one of `x86.fpe pe`.
void checks(const Def* def, DefVec& conds, DefSet& done) {
    if (def->isa_mut() || !done.emplace(def).second) return;
    if (auto check = Axm::isa<refly::check>(def)) conds.emplace_back(check->arg(3, 0));
    for (auto op : def->ops())
        checks(op, conds, done);
}

/// The summed x86::insn::cost of the instructions in @p def.
size_t cost(const Def* def, DefSet& done) {
    if (def->isa_mut() || !done.emplace(def).second) return 0;
    size_t n = 0;
    if (auto [axm, curry, _] = Axm::get(def); axm && curry == 0 && axm->plugin() == x86::Plugin_Id)
        n += x86::insn::cost(def->as<App>());
    for (auto op : def->ops())
        n += cost(op, done);
    return n;
}

size_t cost(const Def* def) {
    auto done = DefSet();
    return cost(def, done);
}

/// A ref with its owner, the node it is an operand of (0 for the root, `k + 1` for cover `k`).
struct RefInfo {
    u8 cover;
    u8 owner;
    Ref kind;
};

/// The shape of an lhs that a matcher records; covers are in walk order until `Code::match` puts them into preorder.
struct Shape {
    std::vector<u32> covers;
    std::vector<RefInfo> refs;
    std::vector<x86::rules::Use> uses;
    std::vector<std::pair<u32, u32>> swaps; ///< The operands of each commuting node.
};

class Table {
public:
    /// The axioms in @p def whose two operands a rule may also match swapped.
    void commutes(const Def* def) {
        for (auto op : def->projs())
            commutes_.emplace(op->as<Axm>()->flags());
    }

    /// The values of an implicit family parameter of type `«2; Nat»` that a `refly.check` in its rule's type mentions.
    void formats(const Def* def) {
        for (auto op : def->projs())
            formats_.emplace_back(op);
    }

    void collect(const Def* def) {
        if (auto rule = def->isa<Rule>())
            add(rule);
        else if (auto lam = def->isa_mut<Lam>(); lam && lam->type()->is_implicit())
            family(lam);
        else if (auto tuple = def->isa<Tuple>())
            for (auto op : tuple->ops())
                collect(op);
    }

    /// Puts the rewrites with the same root next to each other, in their order.
    void sort() { std::ranges::stable_sort(rewrites_, {}, &Rewrite::root); }
    void dump(std::ostream& os) const;

private:
    /// Rejects a rule set that cannot cover every program: each root needs an atomic rule and each hole a filler.
    void check(const std::vector<Shape>&, const std::vector<size_t>& ids) const;

    struct Node {
        Kind kind;
        u64 val = 0;
        std::vector<u32> ops;
        bool operator==(const Node&) const = default;

        struct Hash {
            size_t operator()(const Node& n) const noexcept {
                auto h = fe::hash_combine(size_t(n.kind), n.val);
                for (auto op : n.ops)
                    h = fe::hash_combine(h, op);
                return h;
            }
        };
    };

    struct Meta {
        Want want;
        u32 type;
    };

    struct Conversion {
        std::string name;
        std::string rho;
        size_t cost;
        Ty from, to;
        u32 rhs;
        size_t arity;
    };

    struct Rewrite {
        std::string name;
        std::string rho;
        size_t cost;
        flags_t root;
        Ty type;
        u32 lhs, rhs;
        u32 mapped; ///< The memory Extract the rhs replaces, or `u32(-1)`.
        u32 tied;   ///< The meta variables that a two-address instruction of the rhs overwrites.
        std::vector<Meta> metas;
    };

    void family(Lam*);
    void add(const Rule*, const Var* param = nullptr);
    std::vector<Meta> metas(const Rule*);
    void guard(const Def*, std::vector<Meta>&);
    u32 node(const Def*);
    u32 node_(const Def*);
    u32 make(Node, std::string comment = {});
    u32 hole(u64 i) { return make({Kind::Hole, i, {}}); }
    /// The meta variable @p meta under a marker, as an operand of type @p want.
    u32 marked(const Def* meta, Want want) {
        auto id = node(meta);
        if (nodes_[id].kind != Kind::Hole) meta->blame("x86_isel: `{}` must be a meta variable", meta).bail();
        marks_[nodes_[id].val] = want;
        return id;
    }
    /// The register operand @p op, which may also be an immediate of type `x86::rules::literal_of(want)`.
    u32 widened(const Def* op, Want want) {
        auto id = node(op);
        if (nodes_[id].kind != Kind::Hole) op->blame("x86_isel: `{}` must be a meta variable", op).bail();
        marks_[nodes_[id].val] = want;
        return id;
    }

    size_t num_metas() const { return metas_.size(); }
    /// The meta variable @p def is, under its markers.
    std::optional<size_t> meta(const Def* def) const {
        for (;;) {
            auto [axm, curry, _] = Axm::get(def);
            if (!axm || curry != 0 || axm->plugin() != Plugin_Id) break;
            def = def->as<App>()->arg();
        }
        if (auto i = fe::lookup(meta_ids_, def)) return *i;
        return {};
    }
    u32 tied(const Def* rhs) const;

    DefSet seen_;
    DefVec formats_;
    ankerl::unordered_dense::map<u64, Want> marks_; ///< The marker of each meta variable of the current rule.
    const Var* var_ = nullptr;                      ///< The Var of the current rule.
    DefVec metas_; ///< The meta variables of the current rule: a family parameter, then the Var's projections.
    DefMap<size_t> meta_ids_;
    DefMap<u32> def2node_;
    ankerl::unordered_dense::map<Node, u32, Node::Hash> node2id_;
    std::vector<Node> nodes_;
    std::vector<std::string> comments_;
    std::vector<Rewrite> rewrites_;
    std::vector<Conversion> conversions_;
    ankerl::unordered_dense::set<flags_t> commutes_;
    friend class Code;
    friend std::string sig(const Table&, u32, const std::vector<Meta>&, bool);
};

/// Adds the Rule in the body of the implicit family @p lam.
/// It is instantiated at each of `formats` that the `refly.check`s in its type admit if they mention the family
/// parameter, and the parameter is a meta variable of it otherwise.
void Table::family(Lam* lam) {
    if (!seen_.emplace(lam).second) return;
    auto rule = lam->body()->isa<Rule>();
    if (!rule) lam->blame("x86_isel: the body of the family `{}` is no rule", lam).bail();
    auto param = lam->has_var();
    if (!param) return add(rule);

    auto conds = DefVec();
    auto done  = DefSet();
    checks(rule->type()->dom(), conds, done);
    checks(rule->type()->codom(), conds, done);
    if (std::ranges::none_of(conds, [&](const Def* c) { return c->has_free_var(param); })) return add(rule, param);

    auto& w = rule->world();
    auto pe = w.arr(2, w.type_nat());
    auto ty = lam->dom();
    auto k  = ty == pe ? 1 : ty->isa<Sigma>() ? ty->num_ops() : 0;
    for (size_t j = 0; j != k; ++j)
        if (k != 1 && ty->op(j) != pe) k = 0;
    if (k == 0) lam->blame("x86_isel: no values to instantiate `{}` of `{}` with", ty, lam).bail();

    auto ix = std::vector<size_t>(k);
    for (;;) {
        auto vals = DefVec(k, [&](size_t j) { return formats_[ix[j]]; });
        auto arg  = k == 1 ? vals.front() : w.tuple(vals);
        auto ok   = true;
        for (auto cond : conds) {
            auto c = Lit::isa<bool>(VarRewriter(param, arg).rewrite(cond));
            if (!c) lam->blame("x86_isel: cannot decide `{}` of `{}` at `{}`", cond, lam, arg).bail();
            ok &= *c;
        }
        if (ok) add(lam->reduce_body(arg)->as<Rule>());

        size_t j = k;
        while (j-- != 0 && ++ix[j] == formats_.size())
            ix[j] = 0;
        if (j == size_t(-1)) break;
    }
}

/// Adds @p rule, whose meta variables are preceded by the family parameter @p param, if any.
void Table::add(const Rule* rule, const Var* param) {
    if (!seen_.emplace(rule).second) return;
    auto fvs = rule->free_vars();
    if (!(fvs.empty() || (param && fvs.size() == 1 && fvs.contains(param))))
        rule->blame("x86_isel: rule `{}` is not closed", rule).bail();
    if (rule->guard() == rule->world().lit_ff()) return;

    var_ = rule->has_var();
    metas_.clear();
    meta_ids_.clear();
    if (param) metas_.emplace_back(param);
    if (var_) {
        auto n = var_->num_projs();
        for (size_t i = 0; i != n; ++i)
            metas_.emplace_back(n == 1 ? var_ : var_->proj(n, i));
    }
    for (size_t i = 0; i != metas_.size(); ++i)
        meta_ids_[metas_[i]] = i;
    marks_.clear();
    auto lhs    = rule->lhs();
    auto mapped = u32(-1);
    if (var_ && lhs == var_) {
        auto from = var_->type();
        if (!Idx::isa(from) && classify(from) == Want::Reg)
            rule->blame("x86_isel: the meta variable of conversion `{}` must be a register or a non-terminal", rule)
                .bail();
        // `x86_isel.lit x` makes it a conversion of a literal.
        auto lit     = Axm::isa<x86_isel::lit>(rule->guard());
        bool literal = lit && lit->arg() == var_;
        if (!literal && rule->guard() != rule->world().lit_tt())
            rule->blame("x86_isel: the guard of conversion `{}` is no `x86_isel.lit` of its meta variable", rule)
                .bail();
        conversions_.emplace_back(Conversion{.name  = rule->sym().str(),
                                             .rho   = std::format("{} ↝ {}", rule->dom(), rule->codom()),
                                             .cost  = cost(rule->rhs()),
                                             .from  = literal ? Ty::Lit : nonterminal(rule, from),
                                             .to    = type(rule),
                                             .rhs   = node(rule->rhs()),
                                             .arity = num_metas()});
        var_ = nullptr;
        return;
    }
    // `(m, e)`: the memory `m` of an inner node of `e` becomes that of the rhs.
    if (auto t = lhs->isa<Tuple>(); t && t->num_ops() == 2 && core::isa_mem(t->op(0))) {
        mapped = node(t->op(0));
        lhs    = t->op(1);
    }
    auto [root, _, __] = Axm::get(lhs);
    if (!root && !lhs->isa<Extract>())
        rule->blame("x86_isel: the lhs of `{}` must apply an axiom or extract", rule).bail();

    // A family that takes the operation names all of its instances alike.
    auto name = root ? std::format("{}({})", rule->sym().str(), root->sym().str()) : rule->sym().str();
    rewrites_.emplace_back(Rewrite{.name   = std::move(name),
                                   .rho    = std::format("{} ↝ {}", rule->dom(), rule->codom()),
                                   .cost   = cost(rule->rhs()),
                                   .root   = root ? root->flags() : 0,
                                   .type   = type(rule),
                                   .lhs    = node(lhs),
                                   .rhs    = node(rule->rhs()),
                                   .mapped = mapped,
                                   .tied   = tied(rule->rhs()),
                                   .metas  = metas(rule)});
    var_ = nullptr;
}

u32 Table::tied(const Def* rhs) const {
    u32 res   = 0;
    auto done = DefSet();
    auto todo = DefVec{rhs};
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if (def->isa_mut() || !done.emplace(def).second) continue;
        if (auto [axm, curry, _] = Axm::get(def); axm && curry == 0 && axm->plugin() == x86::Plugin_Id)
            if (auto i = x86::insn::tied(def->as<App>())) {
                auto arg = def->as<App>()->arg();
                if (auto m = meta(arg->proj(arg->num_projs(), *i))) res |= 1u << *m;
            }
        for (auto op : def->ops())
            todo.emplace_back(op);
    }
    return res;
}

std::vector<Table::Meta> Table::metas(const Rule* rule) {
    auto res = std::vector<Meta>();
    auto n   = num_metas();
    for (size_t i = 0; i != n; ++i) {
        auto meta  = metas_[i];
        auto type  = meta->type();
        bool param = !uses(rule->rhs(), meta, var_);
        for (size_t j = 0; j != n && !param; ++j)
            param = j != i && uses(metas_[j]->type(), meta, var_);

        auto want = Want::Param;
        if (!param) {
            auto mark   = marks_.find(i);
            auto marked = [&](Want dflt) { return mark == marks_.end() ? dflt : mark->second; };
            if (Idx::isa(type)) {
                want = marked(Want::Reg);
            } else if (math::isa_f(type)) {
                want = marked(Want::FReg);
            } else if (type->isa<Nat>()) {
                want = Want::Param;
            } else if (Axm::isa<core::Ptr>(type)) {
                if (mark == marks_.end())
                    rule->blame("x86_isel: pointer `{}` of `{}` is no `x86_isel.addr`", meta, rule).bail();
                want = mark->second;
            } else if (Axm::isa<core::M>(type) || type->isa<Pi>()) {
                want = Want::Param;
            } else if (Axm::isa<x86::Addr>(type)) {
                want = classify(type);
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

    auto set = [&](const Def* meta, Want from, Want to) {
        for (size_t i = 0; i != num_metas(); ++i)
            if (metas_[i] == meta && metas[i].want == from) {
                metas[i].want = to;
                return;
            }
        g->blame("x86_isel: guard `{}` must constrain an operand of type `Idx s` or, for `lit`, a `Nat`", g).bail();
    };

    if (auto imm = Axm::isa<x86_isel::imm>(g))
        set(imm->arg(), Want::Reg, Want::Imm);
    else if (auto lit = Axm::isa<x86_isel::lit>(g))
        set(lit->arg(), Want::Param, Want::Lit);
    else
        g->blame("x86_isel: guard `{}` is no conjunction of `x86_isel.imm` and `x86_isel.lit`", g).bail();
}

u32 Table::node(const Def* def) {
    if (auto id = fe::lookup(def2node_, def)) return *id;
    auto id               = node_(def);
    return def2node_[def] = id;
}

u32 Table::node_(const Def* def) {
    if (auto i = fe::lookup(meta_ids_, def)) return hole(*i);
    if (var_ && def == var_) {
        auto ops = std::vector<u32>();
        for (size_t i = num_metas() - var_->num_projs(); i != num_metas(); ++i)
            ops.emplace_back(hole(i));
        return make({Kind::Tuple, 0, std::move(ops)});
    }

    if (auto addr = Axm::isa<x86_isel::addr>(def)) return marked(addr->arg(), Want::Addr);
    if (auto ri = Axm::isa<x86_isel::reg_imm>(def)) return widened(ri->arg(), Want::RegImm);
    if (auto rs = Axm::isa<x86_isel::reg_sh>(def)) return widened(rs->arg(), Want::RegSh);
    if (auto scaled = Axm::isa<x86_isel::scaled>(def)) return marked(scaled->arg(), Want::Scaled);
    if (auto axm = def->isa<Axm>()) return make({Kind::Axm, axm->flags(), {}}, axm->sym().str());
    if (auto lit = def->isa<Lit>()) return make({Kind::Lit, lit->get(), {node(lit->type())}});
    if (def->isa<Nat>()) return make({Kind::Nat, 0, {}});
    if (def->isa<Idx>()) return make({Kind::Idx, 0, {}});
    if (auto app = def->isa<App>()) {
        auto [axm, curry, _] = Axm::get(app);
        u64 flags            = app->callee_type()->is_implicit() ? u64(Implicit) : 0;
        auto arg             = node(app->arg());
        if (axm && curry == 0 && !app->type()->isa<Type>()) {
            flags |= Inner;
            // The arg may also be the meta variables as a whole.
            if (commutes_.contains(axm->flags()) && nodes_[arg].kind == Kind::Tuple && nodes_[arg].ops.size() == 2)
                flags |= Commutes;
        }
        return make({Kind::App, flags, {node(app->callee()), arg}});
    }
    if (auto ex = def->isa<Extract>()) {
        auto flags = core::isa_mem(ex) ? Mem : 0;
        return make({Kind::Extract, flags, {node(ex->tuple()), node(ex->index())}});
    }
    if (auto pack = def->isa_imm<Pack>()) return make({Kind::Pack, 0, {node(pack->arity()), node(pack->body())}});
    if (auto arr = def->isa_imm<Arr>()) return make({Kind::Arr, 0, {node(arr->arity()), node(arr->body())}});
    if (auto pi = def->isa_imm<Pi>()) return make({Kind::Pi, pi->is_implicit(), {node(pi->dom()), node(pi->codom())}});
    if (def->isa<Bot>()) return make({Kind::Bot, 0, {}});

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

    Shape match(size_t id, u32 lhs, std::span<const u32> types);
    void build(u32 rhs);

private:
    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::println(os_, "    {}", std::format(fmt, std::forward<Args>(args)...));
    }

    void walk(u32 lhs);
    std::string fresh(char prefix) { return std::format("{}{}", prefix, next_++); }
    const Table::Node& node(u32 n) const { return t_.nodes_[n]; }
    const std::string& comment(u32 n) const { return t_.comments_[n]; }
    /// Is the type @p n a `core.Ptr`, whose casts the matcher looks through?
    bool is_ptr(u32 n) const {
        const auto& x = node(n);
        if (x.kind != Kind::App) return false;
        const auto& f = node(x.ops[0]);
        return f.kind == Kind::Axm && f.val == Annex::base<core::Ptr>();
    }
    bool inner(u32 n) const { return !in_type_ && node(n).kind == Kind::App && node(n).val & Inner && n != root_; }
    /// Binds @p expr to a fresh variable, unless it is one.
    std::string name(const std::string& expr);
    /// Matches @p n against @p expr; on the root's callee @p spine, isel already checked the axiom.
    void match(u32 n, const std::string& expr, bool spine = false);
    void match_inner(u32 n, const std::string& expr);
    void match_app(u32 n, const std::string& expr, bool spine = false);
    void match2(u32 n, const std::string& expr, std::string_view isa, std::string_view op0, std::string_view op1);
    void ref(u32 n, const std::string& expr, Ref kind);
    std::string build_(u32 n);

    std::ostream& os_;
    const Table& t_;
    size_t next_  = 0;
    bool in_type_ = false;
    u32 root_     = 0;
    u8 owner_     = 0;
    std::vector<bool> bound_;
    ankerl::unordered_dense::map<u32, std::string> cover_vars_;
    std::span<const u32> types_;
    ankerl::unordered_dense::map<u32, std::string> built_;
    Shape shape_;
};

Shape Code::match(size_t id, u32 lhs, std::span<const u32> types) {
    types_ = types;
    std::println(os_,
                 "inline bool match_{}(const Def* def, [[maybe_unused]] u32 swap, [[maybe_unused]] const Def** m, "
                 "[[maybe_unused]] const Def** r) {{",
                 id);
    walk(lhs);
    line("return true;");
    std::println(os_, "}}");
    std::println(os_);

    // Number the covers in preorder of the Value refs, so a cover's operands follow it.
    auto children = std::vector<std::vector<u8>>(shape_.covers.size() + 1);
    for (const auto& r : shape_.refs)
        if (r.kind == Ref::Value) children[r.owner].emplace_back(r.cover);
    auto perm  = std::vector<u8>(shape_.covers.size(), u8(-1));
    auto stack = std::vector<u8>(children[0].rbegin(), children[0].rend());
    u8 next    = 0;
    while (!stack.empty()) {
        auto c = stack.back();
        stack.pop_back();
        if (perm[c] != u8(-1)) continue;
        perm[c] = next++;
        stack.insert(stack.end(), children[c + 1].rbegin(), children[c + 1].rend());
    }
    if (next != shape_.covers.size()) fe::throwf("x86_isel: an inner node is no operand of its rule");
    auto res   = std::move(shape_);
    auto owner = [&](u8 o) { return o == 0 ? o : u8(perm[o - 1] + 1); };
    for (auto& r : res.refs)
        r.cover = perm[r.cover], r.owner = owner(r.owner);
    for (auto& u : res.uses)
        u.owner = owner(u.owner);
    auto covers = res.covers;
    for (size_t k = 0; k != covers.size(); ++k)
        res.covers[perm[k]] = covers[k];
    return res;
}

void Code::walk(u32 lhs) {
    next_  = 0;
    owner_ = 0;
    root_  = lhs;
    shape_ = {};
    cover_vars_.clear();
    bound_.assign(types_.size(), false);
    match(lhs, "def", true);
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

void Code::ref(u32 n, const std::string& e, Ref kind) {
    auto k = std::ranges::find(shape_.covers, n) - shape_.covers.begin();
    if (size_t(k) == shape_.covers.size()) shape_.covers.emplace_back(n);
    auto j = shape_.refs.size();
    shape_.refs.emplace_back(RefInfo{u8(k), owner_, kind});
    if (kind == Ref::Value) shape_.uses.emplace_back(x86::rules::Use{owner_, u8(j), true});
    line("r[{}] = {};", j, e);
}

void Code::match_inner(u32 n, const std::string& e) {
    if (auto var = cover_vars_.find(n); var != cover_vars_.end()) {
        line("if ({} != {}) return false;", e, var->second);
        return;
    }
    auto var       = name(e);
    cover_vars_[n] = var;
    auto outer     = owner_;
    owner_         = u8(std::ranges::find(shape_.covers, n) - shape_.covers.begin() + 1);
    match_app(n, var);
    owner_ = outer;
}

void Code::match_app(u32 n, const std::string& e, bool spine) {
    const auto& x = node(n);
    auto app      = fresh('d');
    line("auto {} = {}->isa<App>();", app, e);
    line("if (!{}) return false;", app);
    match(x.ops[0], app + "->callee()", spine);
    if (in_type_ || !(x.val & Commutes)) return match(x.ops[1], app + "->arg()");
    auto arg = name(app + "->arg()");
    auto bit = shape_.swaps.size();
    auto& t  = node(x.ops[1]);
    shape_.swaps.emplace_back(t.ops[0], t.ops[1]);
    line("if (Lit::isa({}->arity()) != 2) return false;", arg);
    match(t.ops[0], std::format("{}->proj(2, swap >> {} & 1)", arg, bit));
    match(t.ops[1], std::format("{}->proj(2, ~swap >> {} & 1)", arg, bit));
}

void Code::match(u32 n, const std::string& e, bool spine) {
    const auto& x = node(n);
    switch (x.kind) {
        case Kind::Hole: {
            auto i = x.val;
            if (!in_type_) shape_.uses.emplace_back(x86::rules::Use{owner_, u8(i), false});
            auto ptr = is_ptr(types_[i]);
            if (bound_[i]) {
                line("if (m[{}] != {}) return false;", i, ptr ? std::format("strip_ptr_cast({})", e) : e);
                return;
            }
            bound_[i] = true;
            line("m[{}] = {};", i, e);
            auto outer = std::exchange(in_type_, true);
            match(types_[i], std::format("m[{}]->type()", i));
            in_type_ = outer;
            if (ptr) line("m[{0}] = strip_ptr_cast(m[{0}]);", i);
            return;
        }
        case Kind::Axm: {
            if (spine) return;
            auto d = name(e);
            line("if (!{0}->isa<Axm>() || {0}->flags() != {1:#x}) return false; // {2}", d, x.val, comment(n));
            return;
        }
        case Kind::Lit: {
            auto lit = fresh('d');
            line("auto {} = {}->isa<Lit>();", lit, e);
            line("if (!{0} || {0}->get() != {1:#x}) return false;", lit, x.val);
            auto outer = std::exchange(in_type_, true);
            match(x.ops[0], lit + "->type()");
            in_type_ = outer;
            return;
        }
        case Kind::Nat: line("if (!{}->isa<Nat>()) return false;", e); return;
        case Kind::Idx: line("if (!{}->isa<Idx>()) return false;", e); return;
        case Kind::App: {
            if (!inner(n)) return match_app(n, e, spine);
            auto d = name(e);
            if (!cover_vars_.contains(n)) ref(n, d, Ref::Value);
            return match_inner(n, d);
        }
        case Kind::Extract: {
            auto tuple = x.ops[0];
            if (!inner(tuple)) return match2(n, e, "isa<Extract>()", "tuple", "index");
            auto ex = fresh('d');
            line("auto {} = {}->isa<Extract>();", ex, e);
            line("if (!{}) return false;", ex);
            ref(tuple, ex, x.val & Mem ? Ref::Mem : Ref::Value);
            match_inner(tuple, ex + "->tuple()");
            match(x.ops[1], ex + "->index()");
            return;
        }
        case Kind::Pack: return match2(n, e, "isa_imm<Pack>()", "arity", "body");
        case Kind::Arr: return match2(n, e, "isa_imm<Arr>()", "arity", "body");
        case Kind::Pi: return match2(n, e, "isa_imm<Pi>()", "dom", "codom");
        case Kind::Bot: line("if (!{}->isa<Bot>()) return false;", e); return;
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
    std::println(os_, "inline const Def* build_{}([[maybe_unused]] World& w, [[maybe_unused]] const Def* const* h) {{",
                 rhs);
    line("return {};", build_(rhs));
    std::println(os_, "}}");
    std::println(os_);
}

std::string Code::build_(u32 n) {
    if (auto i = built_.find(n); i != built_.end()) return i->second;
    const auto& x = node(n);
    if (x.kind == Kind::Hole) return built_[n] = std::format("h[{}]", x.val);

    // The holes determine the implicit arguments, which may differ from the rule's, e.g. `i` of an `x86.Addr i`.
    const auto& callee = node(x.kind == Kind::App ? x.ops[0] : n);
    if (x.kind == Kind::App && callee.kind == Kind::App && callee.val & Implicit) {
        auto f   = build_(callee.ops[0]);
        auto a   = build_(x.ops[1]);
        auto res = fresh('b');
        line("auto {} = w.implicit_app({}, {});", res, f, a);
        return built_[n] = res;
    }

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
        case Kind::Pi: line("auto {} = w.pi({}, {}, {});", res, ops[0], ops[1], x.val != 0); break;
        case Kind::Bot: line("auto {} = w.bot(w.type());", res); break;
        case Kind::Hole: std::unreachable();
    }
    return built_[n] = res;
}

/// Hashes an lhs followed by the types of its meta variables.
struct LhsHash {
    size_t operator()(const std::vector<u32>& v) const noexcept {
        size_t h = 0;
        for (auto x : v)
            h = fe::hash_combine(h, x);
        return h;
    }
};

/// The signature of the subpattern @p n under @p metas: equal ones match the same terms.
/// If @p reg, a RegImm or RegSh hole is a Reg.
std::string sig(const Table& t, u32 n, const std::vector<Table::Meta>& metas, bool reg);

void Table::check(const std::vector<Shape>& shapes, const std::vector<size_t>& ids) const {
    auto chains = x86::rules::Chains(conversions_);
    auto atomic = ankerl::unordered_dense::set<flags_t>();
    for (size_t i = 0, e = rewrites_.size(); i != e; ++i) {
        const auto& r = rewrites_[i];
        if (chains.conv(r.type, x86::rules::reg_want(r.type)) == x86::rules::Inf)
            fe::throwf("x86_isel: rule `{}` yields a value that does not convert to a register", r.name);
        for (auto m : r.metas)
            if (!x86::rules::is_param(m.want) && !x86::rules::is_literal(m.want) && !chains.reachable(m.want))
                fe::throwf("x86_isel: no rule type converts to a hole of `{}`", r.name);
        auto total = [&](Meta m) { return x86::rules::is_param(m.want) || chains.total(m.want); };
        if (shapes[ids[i]].covers.empty() && std::ranges::all_of(r.metas, total)) atomic.emplace(r.root);
    }
    for (const auto& r : rewrites_)
        if (!atomic.contains(r.root)) fe::throwf("x86_isel: no atomic rule shares the root of `{}`", r.name);
}

void Table::dump(std::ostream& os) const {
    std::println(os, "// Generated by `x86_isel.gen`; do not edit.");
    std::println(os, "#pragma once");
    std::println(os);
    std::println(os, "#include <mim/world.h>");
    std::println(os);
    std::println(os, "#include \"mim/plug/x86/rule_table.h\"");
    std::println(os, "#include \"mim/plug/x86/x86.h\"");
    std::println(os);
    std::println(os, "namespace mim::plug::x86::rules {{");
    std::println(os);

    auto code     = Code(os, *this);
    auto matchers = ankerl::unordered_dense::map<std::vector<u32>, size_t, LhsHash>();
    auto shapes   = std::vector<Shape>();
    auto ids      = std::vector<size_t>();
    for (const auto& r : rewrites_) {
        auto types = std::vector<u32>{r.lhs};
        for (auto m : r.metas)
            types.emplace_back(m.type);
        auto [i, fresh] = matchers.emplace(types, matchers.size());
        if (fresh) shapes.emplace_back(code.match(i->second, r.lhs, std::span(types).subspan(1)));
        ids.emplace_back(i->second);
    }
    check(shapes, ids);

    auto built = ankerl::unordered_dense::set<u32>();
    for (const auto& r : rewrites_)
        if (built.emplace(r.rhs).second) code.build(r.rhs);
    for (const auto& c : conversions_)
        if (built.emplace(c.rhs).second) code.build(c.rhs);

    std::println(os, "// clang-format off");
    std::println(os, "inline constexpr Want wants[] = {{");
    size_t num_wants = 0;
    for (const auto& r : rewrites_) {
        if (r.metas.empty()) continue;
        std::print(os, "   ");
        for (auto m : r.metas)
            std::print(os, " Want::{},", name(m.want));
        std::println(os, " // {}: {}", r.name, r.rho);
        num_wants += r.metas.size();
    }
    // An empty array is ill-formed.
    if (num_wants == 0) std::println(os, "    Want::Param,");
    std::println(os, "}};");
    std::println(os);

    constexpr std::string_view refs[] = {"Value", "Mem"};
    std::println(os, "inline constexpr Use all_uses[] = {{");
    std::println(os, "    {{0, 0, false}},");
    for (const auto& s : shapes)
        for (auto u : s.uses)
            std::println(os, "    {{{}, {}, {}}},", u.owner, u.index, u.ref);
    std::println(os, "}};");
    std::println(os);
    std::println(os, "inline constexpr RefInfo all_refs[] = {{");
    std::println(os, "    {{0, Ref::Value}},");
    for (const auto& s : shapes)
        for (auto r : s.refs)
            std::println(os, "    {{{}, Ref::{}}},", r.cover, refs[size_t(r.kind)]);
    std::println(os, "}};");
    std::println(os);
    auto max = [&](auto f) { return std::ranges::max(shapes | std::views::transform(f)); };
    std::println(os, "inline constexpr size_t Max_Refs   = {};", max([](const Shape& s) { return s.refs.size(); }));
    std::println(os, "inline constexpr size_t Max_Covers = {};", max([](const Shape& s) { return s.covers.size(); }));
    std::println(os, "inline constexpr size_t Max_Metas  = {};",
                 std::ranges::max(rewrites_ | std::views::transform([](const Rewrite& r) { return r.metas.size(); })));
    std::println(os);
    std::println(os, "inline constexpr Matcher matchers[] = {{");
    size_t num_uses = 1, num_refs = 1;
    for (size_t i = 0; i != shapes.size(); ++i) {
        const auto& s = shapes[i];
        std::println(os, "    {{&match_{}, {}, {}, {}, {}, {}}},", i, num_uses, s.uses.size(), num_refs, s.refs.size(),
                     s.covers.size());
        num_uses += s.uses.size();
        num_refs += s.refs.size();
    }
    std::println(os, "}};");
    std::println(os);

    std::println(os, "inline constexpr Rewrite rewrites[] = {{");
    size_t first = 0;
    for (size_t i = 0, e = rewrites_.size(); i != e; ++i) {
        const auto& r = rewrites_[i];
        const auto& s = shapes[ids[i]];
        u32 swaps = 0, reg_swaps = 0;
        for (size_t b = 0; b != s.swaps.size(); ++b) {
            auto [x, y] = s.swaps[b];
            if (sig(*this, x, r.metas, false) != sig(*this, y, r.metas, false)) swaps |= 1u << b;
            if (sig(*this, x, r.metas, true) != sig(*this, y, r.metas, true)) reg_swaps |= 1u << b;
        }
        size_t maps = 0;
        if (r.mapped != u32(-1)) {
            auto k = std::ranges::find(s.covers, nodes_[r.mapped].ops[0]) - s.covers.begin();
            if (size_t(k) == s.covers.size()) fe::throwf("x86_isel: `{}` maps the memory of no inner node", r.name);
            maps = k + 1;
        }
        std::println(
            os, "    {{\"{}\", {}, {:#x}, Ty::{}, &matchers[{}], &build_{}, {}, {}, {:#x}, {:#x}, {:#x}, {}}}, // {}",
            r.name, r.cost, r.root, name(r.type), ids[i], r.rhs, first, r.metas.size(), swaps, swaps & ~reg_swaps,
            r.tied, maps, r.rho);
        first += r.metas.size();
    }
    std::println(os, "}};");
    std::println(os);
    std::println(os, "inline constexpr Root roots[] = {{");
    for (size_t i = 0, e = rewrites_.size(); i != e;) {
        auto j = i;
        while (j != e && rewrites_[j].root == rewrites_[i].root)
            ++j;
        std::println(os, "    {{{:#x}, {}, {}}},", rewrites_[i].root, i, j - i);
        i = j;
    }
    std::println(os, "}};");
    std::println(os);
    // An empty array is ill-formed.
    if (conversions_.empty()) fe::throwf("x86_isel: there is no conversion");
    std::println(os, "inline constexpr Conversion conversions[] = {{");
    for (const auto& c : conversions_)
        std::println(os, "    {{\"{}\", {}, Ty::{}, Ty::{}, {}, &build_{}}}, // {}", c.name, c.cost, name(c.from),
                     name(c.to), c.arity, c.rhs, c.rho);
    std::println(os, "}};");
    std::println(os);
    std::println(os, "inline constexpr Chains chains{{conversions}};");
    std::println(os, "// clang-format on");
    std::println(os);
    std::println(os, "}} // namespace mim::plug::x86::rules");
}

std::string sig(const Table& t, u32 n, const std::vector<Table::Meta>& metas, bool reg) {
    const auto& x = t.nodes_[n];
    if (x.kind == Kind::Hole) {
        auto w = metas[x.val].want;
        if (reg && x86::rules::literal_of(w) != w) w = Want::Reg;
        return std::format("{}:{}", name(w), sig(t, metas[x.val].type, metas, reg));
    }
    auto res = std::format("{}.{}(", size_t(x.kind), x.val);
    for (auto op : x.ops)
        res += sig(t, op, metas, reg) + ",";
    return res + ")";
}

} // namespace

void Gen::start() {
    auto& lib  = *driver().library();
    auto flags = std::vector<flags_t>();
    for (const auto& [f, _] : lib.annexes())
        if (Annex::flags2plugin(f) == Plugin_Id) flags.emplace_back(f);
    std::ranges::sort(flags);

    auto table = Table();
    table.commutes(world().annex<commutes>());
    table.formats(world().annex<formats>());
    for (auto f : flags)
        table.collect(world().annex(f));
    table.sort();

    auto out = Out(std::string(arg_value(args(), "o", "output").value_or("-")));
    table.dump(*out.os());
}

} // namespace mim::plug::x86_isel
