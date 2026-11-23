#include <print>
#include <ranges>

#include <pbqp/dot.h>
#include <pbqp/solver.h>

#include <mim/liveness.h>
#include <mim/rewrite.h>

#include "mim/native/regalloc.h"

namespace mim::native {

namespace {

constexpr pbqp::Cost Move = 4;
// A callee-saved register costs a save and a restore once per function, not per value.
constexpr pbqp::Cost Callee_Saved = 1;

class Prune : public Rewriter {
public:
    Prune(const Target& target, World& world, const DefSet& dead)
        : Rewriter(world)
        , target_(target)
        , dead_(dead) {}

    const Def* rewrite(const Def* def) final {
        if (def->isa_mut() || def->is_closed()) return def;
        if (auto ex = def->isa<Extract>(); ex && target_.is_mem(def))
            if (auto app = ex->tuple()->isa<App>(); app && dead_.contains(app)) return rewrite(target_.prev_mem(app));
        return Rewriter::rewrite(def);
    }

private:
    const Target& target_;
    const DefSet& dead_;
};

void prune(const Target& target, Lam* fun) {
    auto live = Liveness(target, fun);
    // Rewriting a jump in place must not inline its target, e.g., once pruning made both targets of a branch equal.
    for (const auto& block : live.blocks()) {
        if (auto lam = block.lam; lam->filter() != fun->world().lit_ff()) {
            auto body = lam->body();
            lam->unset()->set(false, body);
        }
    }

    auto count = DefMap<size_t>();
    for_each_use(live, [&](const Def* u) { ++count[u]; });

    auto unused = [&](const Def* d) {
        auto n = fe::lookup(count, d);
        return !n || *n == 0;
    };
    auto dead = DefSet();
    for (bool todo = true; todo;) {
        todo = false;
        for (const auto& block : live.blocks()) {
            auto ex   = block.lam->body()->as<App>()->callee()->isa<Extract>();
            auto cond = ex ? target.isa_sched_cc(ex->index()) : nullptr;
            for (auto insn : block.insns | std::views::reverse) {
                if (dead.contains(insn) || insn == cond || !target.is_removable(insn)) continue;
                auto ds = live.defs(insn);
                if (ds.empty() || !std::ranges::all_of(ds, unused)) continue;
                dead.emplace(insn);
                todo = true;
                for (auto u : live.uses(insn))
                    --count[u];
            }
        }
    }
    if (dead.empty()) return;

    auto p = Prune(target, fun->world(), dead);
    rewrite_blocks(live, p);
}

class Colorer {
public:
    Colorer(const Target& target, Lam* fun, Liveness::Class cls, std::ostream* dot = nullptr)
        : target_(target)
        , fun_(fun)
        , live_(target, fun)
        , cls_(cls)
        , dot_(dot)
        , n_(target.num_regs(cls))
        , interfere_(n_, n_, [](size_t i, size_t j) { return i == j ? pbqp::Cost::inf() : 0; })
        , tie_(n_, n_, [](size_t i, size_t j) { return i == j ? 0 : pbqp::Cost::inf(); })
        , move_(n_, n_, [](size_t i, size_t j) { return i == j ? 0 : Move; })
        , base_(n_, [&](size_t r) {
            if (target.reserved(cls, r)) return pbqp::Cost::inf();
            return target.caller_saved(cls, r) ? pbqp::Cost(0) : Callee_Saved;
        }) {}

    void run() {
        nodes();
        for (const auto& block : live_.blocks())
            interferences(
                live_, block, [&](const Def* v) { return ids_.contains(v); },
                [&](const Def* a, const Def* b) { link(a, b, interfere_); });
        constraints();
        copies();
        solve();
        auto assign = Assign(fun_->world(), cls_, live_, regs_);
        rewrite_blocks(live_, assign);
    }

private:
    void nodes();
    void constraints();
    void copies();
    void solve();

    bool is_value(const Def* v) const { return mim::is_value(v, cls_, target_); }
    void add(const Def* v);
    pbqp::Costs* costs(const Def* v) {
        auto id = fe::lookup(ids_, target_.strip(v));
        return id ? &costs_[*id] : nullptr;
    }
    void fix(const Def* v, size_t r) {
        if (auto cs = costs(v))
            for (size_t o = 0; o != n_; ++o)
                if (o != r) (*cs)[o] = pbqp::Cost::inf();
    }
    void forbid(const Def* v, size_t r) {
        if (auto cs = costs(v)) (*cs)[r] = pbqp::Cost::inf();
    }
    void prefer(const Def* v, size_t r) {
        if (auto cs = costs(v))
            for (size_t o = 0; o != n_; ++o)
                if (o != r) (*cs)[o] += Move;
    }
    void link(const Def* a, const Def* b, pbqp::MatrixView m) {
        a = target_.strip(a), b = target_.strip(b);
        if (a == b) return;
        if (auto i = fe::lookup(ids_, a))
            if (auto j = fe::lookup(ids_, b)) graph_.add_edge_costs(*i, *j, m);
    }
    void jump(Lam* target, const Def* arg) {
        auto n = target->num_vars();
        for (size_t i = 0; i != n; ++i)
            link(arg->proj(n, i), target->var(n, i), move_);
    }

    const Target& target_;
    Lam* fun_;
    Liveness live_;
    Liveness::Class cls_;
    std::ostream* dot_;
    size_t n_;
    pbqp::Graph graph_;
    DefMap<pbqp::NodeId> ids_;
    DefVec values_;
    fe::Vector<pbqp::Costs> costs_;
    const pbqp::Matrix interfere_, tie_, move_;
    const pbqp::Costs base_;
    DefMap<size_t> regs_;
};

void Colorer::add(const Def* v) {
    ids_[v] = graph_.add(base_);
    values_.emplace_back(v);
    costs_.emplace_back(base_);
}

/// Values are added in an order that respects dominance, so their ids are the PEO for RN.
void Colorer::nodes() {
    auto used = DefSet();
    for_each_use(live_, [&](const Def* u) { used.emplace(u); });

    for (const auto& block : live_.blocks()) {
        for (auto x : live_.params(block))
            if (is_value(x) && used.contains(x)) add(x);
        for (auto insn : block.insns) {
            auto implied = DefSet();
            for (auto [j, _] : target_.constraint(insn).results)
                implied.emplace(Target::result(insn, j));
            for (auto d : live_.defs(insn))
                if (is_value(d) && (used.contains(d) || implied.contains(d))) add(d);
        }
    }
}

void Colorer::constraints() {
    for (const auto& block : live_.blocks()) {
        for (auto insn : block.insns) {
            auto c = target_.constraint(insn);
            // Fixed operands, results, and clobbers of an instruction are general-purpose registers.
            if (cls_ == Liveness::Gpr) {
                for (auto [j, r] : c.fixed)
                    fix(c.ops[j], r);
                for (auto [j, r] : c.results)
                    fix(Target::result(insn, j), r);
                for (auto r : c.clobbers)
                    for (auto u : live_.uses(insn))
                        forbid(u, r);
            }
            if (c.tied && is_value(c.ops[*c.tied])) link(c.ops[*c.tied], Target::result(insn, 0), tie_);
        }
    }
}

void Colorer::copies() {
    auto num_args = target_.num_arg_regs(cls_);
    auto n        = fun_->num_vars();
    for (size_t i = 0, a = 0; i + 1 < n; ++i)
        if (auto x = fun_->var(n, i); is_value(x) && a < num_args) prefer(x, target_.arg_reg(cls_, a++));

    for (const auto& block : live_.blocks()) {
        auto term   = block.lam->body()->as<App>();
        auto callee = term->callee();
        if (callee == fun_->ret_var()) {
            for (auto a : term->args())
                if (is_value(a)) prefer(a, target_.ret_reg(cls_));
        } else if (auto ex = callee->isa<Extract>()) {
            for (auto t : ex->tuple()->projs())
                if (auto bb = Lam::isa_mut_basicblock(t); bb && live_.block(bb)) jump(bb, term->arg());
        } else if (auto bb = Lam::isa_mut_basicblock(callee); bb && live_.block(bb)) {
            jump(bb, term->arg());
        } else if (Lam::isa_mut_returning(callee)) {
            auto m = term->num_args();
            for (size_t i = 0, a = 0; i + 1 < m; ++i)
                if (auto x = term->arg(m, i); is_value(x) && a < num_args) prefer(x, target_.arg_reg(cls_, a++));
            if (auto k = call_cont(live_, fun_, block)) {
                for (auto v : k->live_in)
                    if (auto cs = costs(v))
                        for (size_t r = 0; r != n_; ++r)
                            if (target_.caller_saved(cls_, r)) (*cs)[r] = pbqp::Cost::inf();
                for (auto x : live_.params(*k))
                    if (is_value(x)) prefer(x, target_.ret_reg(cls_));
            }
        }
    }
}

void Colorer::solve() {
    for (size_t i = 0; i != values_.size(); ++i)
        graph_.set_node_costs(i, costs_[i]);

    auto priority = fe::Vector<size_t>(values_.size(), [](size_t i) { return i; });
    auto sol      = pbqp::solve(graph_, priority);
    if (dot_) {
        std::println(*dot_, "// fun {}", fun_);
        for (size_t i = 0; i != values_.size(); ++i)
            std::println(*dot_, "// n{} = {}", i, values_[i]);
        pbqp::write_dot(*dot_, graph_, sol.choices);
    }
    if (!sol.feasible()) {
        auto conflicts = DefVec();
        for (size_t i = 0; i != values_.size(); ++i)
            if (sol.conflicts[i]) conflicts.emplace_back(values_[i]);
        fun_->blame("{} backend: cannot assign registers in `{}`; conflicting values: {}", target_.name(), fun_,
                    fe::Join(conflicts, ", "))
            .bail();
    }
    for (size_t i = 0; i != values_.size(); ++i)
        regs_[values_[i]] = sol.choices[i];
}

/// Checks the invariants of an assignment over a fresh Liveness of @p fun.
class Checker {
public:
    Checker(const Target& target, Lam* fun, Liveness::Class cls)
        : target_(target)
        , fun_(fun)
        , live_(target, fun)
        , cls_(cls) {}

    void run();

private:
    bool is_value(const Def* v) const { return mim::is_value(v, cls_, target_); }
    void walk(const Def* def, const Def* at);
    std::optional<size_t> reg(const Def* v) const {
        if (auto r = fe::lookup(regs_, target_.strip(v))) return *r;
        return std::nullopt;
    }
    [[noreturn]] void fail(const Def* at, std::string msg) {
        at->blame("{} backend: register assignment of `{}`: {}", target_.name(), fun_, msg).bail();
    }

    const Target& target_;
    Lam* fun_;
    Liveness live_;
    Liveness::Class cls_;
    DefMap<size_t> regs_;
};

void Checker::walk(const Def* def, const Def* at) {
    if (def->isa_mut() || def->is_closed() || target_.is_mem(def)) return;
    if (target_.is_in(def)) {
        auto v = target_.strip(def);
        if (target_.cls(v) != cls_) return;
        auto r        = Target::in_reg(def);
        auto [i, ins] = regs_.emplace(v, r);
        if (!live_.local(v) || !is_value(v)) fail(at, std::format("`{}` is no value", v));
        if (target_.reserved(cls_, r)) fail(at, std::format("`{}` is in a reserved register", v));
        if (!ins && i->second != r) fail(at, std::format("`{}` is in two registers", v));
        return;
    }
    if (live_.local(def)) {
        for (auto d : def->projs())
            if (is_value(d)) fail(at, std::format("`{}` is used without a register", d));
        return;
    }
    if (auto app = def->isa<App>(); app && target_.prev_mem(app)) fail(at, std::format("`{}` is off the chain", app));
    for (auto op : def->ops())
        walk(op, at);
}

void Checker::run() {
    for (const auto& block : live_.blocks()) {
        for (auto insn : block.insns) {
            walk(insn->callee(), insn);
            walk(insn->arg(), insn);
        }
        auto term = block.lam->body()->as<App>();
        if (auto ex = term->callee()->isa<Extract>(); ex && target_.isa_sched_cc(ex->index())) {
            walk(ex->tuple(), term);
            walk(term->arg(), term);
        } else {
            walk(term, term);
        }
    }

    for (const auto& block : live_.blocks()) {
        for (auto insn : block.insns) {
            auto c = target_.constraint(insn);
            if (cls_ == Liveness::Gpr) {
                for (auto [j, r] : c.results) {
                    auto d = Target::result(insn, j);
                    if (regs_.emplace(d, r).first->second != r)
                        fail(insn, std::format("`{}` is not in its fixed register", d));
                }
                for (auto [j, r] : c.fixed)
                    if (!target_.is_in(c.ops[j]) || Target::in_reg(c.ops[j]) != r)
                        fail(insn, std::format("operand `{}` is not in its fixed register", c.ops[j]));
                for (auto r : c.clobbers)
                    for (auto u : live_.uses(insn))
                        if (reg(u) == r) fail(insn, std::format("operand `{}` is in a clobbered register", u));
            }
            if (c.tied && is_value(c.ops[*c.tied]) && reg(c.ops[*c.tied]) != reg(Target::result(insn, 0)))
                fail(insn, std::format("`{}` is not in the register of operand `{}`", Target::result(insn, 0),
                                       c.ops[*c.tied]));
        }
    }

    for (const auto& block : live_.blocks()) {
        interferences(
            live_, block, [&](const Def* v) { return regs_.contains(v); },
            [&](const Def* a, const Def* b) {
                if (regs_[a] == regs_[b])
                    fail(block.lam, std::format("`{}` and `{}` interfere but share a register", a, b));
            });
        if (auto k = call_cont(live_, fun_, block))
            for (auto v : k->live_in)
                if (auto r = reg(v); r && target_.caller_saved(cls_, *r))
                    fail(block.lam, std::format("`{}` lives across a call in a caller-saved register", v));
    }
}

} // namespace

void Color::visit(Lam* fun) {
    if (!Lam::isa_mut_returning(fun)) return;

    const auto& t = target();
    prune(t, fun);
    for (auto cls : {Liveness::Gpr, Liveness::Fpr})
        if (t.num_regs(cls) != 0) Colorer(t, fun, cls, dot()).run();
    for (auto cls : {Liveness::Gpr, Liveness::Fpr})
        if (t.num_regs(cls) != 0) Checker(t, fun, cls).run();
}

std::ostream* Color::dot() {
    if (auto d = arg_value(args(), "color-dot")) {
        if (dot_.name().empty()) dot_.name() = std::string(*d);
        return dot_.os();
    }
    return nullptr;
}

} // namespace mim::native
