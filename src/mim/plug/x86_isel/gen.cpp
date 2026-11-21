#include "mim/plug/x86_isel/gen.h"

#include <map>
#include <print>

#include <mim/driver.h>
#include <mim/plugin.h>
#include <mim/rule.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>
#include <mim/plug/x86/autogen.h>
#include <mim/plug/x86/rule_table.h>

#include "mim/plug/x86_isel/x86_isel.h"

namespace mim::plug::x86_isel {

using x86::rules::Kind;
using x86::rules::Want;

namespace {

constexpr std::string_view name(Kind kind) {
    switch (kind) {
#define X(k) \
    case Kind::k: return #k;
        MIM_X86_RULE_KINDS(X)
#undef X
    }
    std::unreachable();
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
        u32 lhs, rhs, metas;
        size_t num_metas;
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
    std::vector<Meta> metas_;
    std::vector<Rewrite> rewrites_;
};

void Table::add(const Rule* rule) {
    if (!seen_.emplace(rule).second) return;
    if (!rule->is_closed()) rule->blame("x86_isel: rule `{}` is not closed", rule).bail();
    if (rule->guard() == rule->world().lit_ff()) return;

    var_       = rule->has_var();
    num_metas_ = var_ ? var_->num_projs() : 0;
    auto metas = this->metas(rule);
    auto first = u32(metas_.size());
    for (auto meta : metas)
        metas_.emplace_back(meta);

    auto done = DefSet();
    rewrites_.emplace_back(Rewrite{.name      = rule->sym().str(),
                                   .type      = std::format("{} ↝ {}", rule->dom(), rule->codom()),
                                   .cost      = num_insns(rule->rhs(), done),
                                   .lhs       = node(rule->lhs()),
                                   .rhs       = node(rule->rhs()),
                                   .metas     = first,
                                   .num_metas = metas.size()});
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

void Table::dump(std::ostream& os) const {
    std::println(os, "// Generated by `x86_isel.gen`; do not edit.");
    std::println(os, "#pragma once");
    std::println(os);
    std::println(os, "#include \"mim/plug/x86/rule_table.h\"");
    std::println(os);
    std::println(os, "namespace mim::plug::x86::rules {{");
    std::println(os);
    std::println(os, "// clang-format off");

    auto ops = std::vector<u32>();
    std::println(os, "inline constexpr Node nodes[] = {{");
    for (size_t i = 0, e = nodes_.size(); i != e; ++i) {
        const auto& n = nodes_[i];
        std::print(os, "    /* {:4} */ {{Kind::{}, {}, {}, {:#x}}},", i, name(n.kind), n.ops.size(), ops.size(), n.val);
        if (!comments_[i].empty()) std::print(os, " // {}", comments_[i]);
        std::println(os);
        ops.insert(ops.end(), n.ops.begin(), n.ops.end());
    }
    std::println(os, "}};");
    std::println(os);

    // An empty array is ill-formed.
    if (ops.empty()) ops.emplace_back(0);
    std::println(os, "inline constexpr u32 ops[] = {{");
    for (size_t i = 0; i < ops.size(); i += 16)
        std::println(os, "    {},", fe::Join(std::span(ops).subspan(i, std::min<size_t>(16, ops.size() - i)), ", "));
    std::println(os, "}};");
    std::println(os);

    std::println(os, "inline constexpr Meta metas[] = {{");
    for (auto [want, type] : metas_)
        std::println(os, "    {{Want::{}, {}}},", name(want), type);
    if (metas_.empty()) std::println(os, "    {{Want::Param, 0}},");
    std::println(os, "}};");
    std::println(os);

    std::println(os, "inline constexpr Rewrite rewrites[] = {{");
    for (const auto& r : rewrites_)
        std::println(os, "    {{\"{}\", {}, {}, {}, {}, {}}}, // {}", r.name, r.cost, r.lhs, r.rhs, r.metas,
                     r.num_metas, r.type);
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
