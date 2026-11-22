#include "mim/plug/x86/phase/color.h"

#include <print>
#include <ranges>

#include <pbqp/dot.h>
#include <pbqp/solver.h>

#include <mim/plugin.h>
#include <mim/rewrite.h>

#include "mim/plug/x86/constraint.h"
#include "mim/plug/x86/liveness.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace {

constexpr pbqp::Cost Move = 4;
// A callee-saved register costs a save and a restore once per function, not per value.
constexpr pbqp::Cost Callee_Saved = 1;

size_t phys(const Def* r) { return *Lit::isa(r); }

bool is_reserved(Liveness::Class cls, size_t r) { return cls == Liveness::Gpr && reserved(r); }

pbqp::Costs base_costs(Liveness::Class cls) {
    return pbqp::Costs(Num_Regs, [cls](size_t r) {
        if (is_reserved(cls, r)) return pbqp::Cost::inf();
        return caller_saved(cls, r) ? pbqp::Cost(0) : Callee_Saved;
    });
}

/// Rewrites the filter and body of each block of @p live in place.
void rewrite_blocks(const Liveness& live, Rewriter& rw) {
    for (const auto& block : live.blocks()) {
        auto lam    = block.lam;
        auto filter = rw.rewrite(lam->filter());
        auto body   = rw.rewrite(lam->body());
        lam->unset()->set(filter, body);
    }
}

/// Calls @p f for each use of a value in @p live.
void for_each_use(const Liveness& live, auto f) {
    for (const auto& block : live.blocks()) {
        for (const auto& uses : block.uses)
            for (auto u : uses)
                f(u);
        for (auto u : block.term_uses)
            f(u);
    }
}

/// The block that a call at the end of @p block returns to, if any.
const Liveness::Block* call_cont(const Liveness& live, Lam* fun, const Liveness::Block& block) {
    auto term = block.lam->body()->as<App>();
    if (term->callee() == fun->ret_var() || !Lam::isa_mut_returning(term->callee())) return nullptr;
    if (auto k = Lam::isa_mut_basicblock(term->arg(term->num_args(), term->num_args() - 1))) return live.block(k);
    return nullptr;
}

/// Calls @p f `(d, v)` for each value `v` that is live at the definition of `d` or defined along with it.
/// Only values that satisfy @p keep take part.
void interferences(const Liveness& live, const Liveness::Block& block, auto keep, auto f) {
    auto alive = DefSet();
    auto gen   = [&](const auto& values) {
        for (auto v : values)
            if (keep(v)) alive.emplace(v);
    };
    auto def = [&](DefVec ds) {
        std::erase_if(ds, [&](const Def* d) { return !keep(d); });
        for (auto d : ds)
            alive.erase(d);
        for (size_t i = 0; i != ds.size(); ++i) {
            for (auto v : alive)
                f(ds[i], v);
            for (size_t j = i + 1; j != ds.size(); ++j)
                f(ds[i], ds[j]);
        }
    };

    gen(block.live_out);
    gen(live.term_uses(block));
    for (auto insn : block.insns | std::views::reverse) {
        def(live.defs(insn));
        gen(live.uses(insn));
    }
    def(live.params(block));
}

class Prune : public Rewriter {
public:
    Prune(World& world, const DefSet& dead)
        : Rewriter(world)
        , dead_(dead) {}

    const Def* rewrite(const Def* def) final {
        if (def->isa_mut() || def->is_closed()) return def;
        if (auto ex = def->isa<Extract>(); ex && is_mem(def))
            if (auto app = ex->tuple()->isa<App>(); app && dead_.contains(app)) return rewrite(prev_mem(app));
        return Rewriter::rewrite(def);
    }

private:
    const DefSet& dead_;
};

void prune(Lam* fun) {
    auto live = Liveness(fun);
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
            auto cond = ex ? isa_sched_cc(ex->index()) : nullptr;
            for (auto insn : block.insns | std::views::reverse) {
                if (dead.contains(insn) || insn == cond) continue;
                if (!Axm::isa<sched>(insn) && !Axm::isa<load>(insn) && !Axm::isa<fload>(insn) && !Axm::isa<rm>(insn)
                    && !Axm::isa<rm1>(insn) && !Axm::isa<slot>(insn))
                    continue;
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

    auto prune = Prune(fun->world(), dead);
    rewrite_blocks(live, prune);
}

/// Wraps every use of a value in `x86.in`, or an XMM value in `x86.fin`.
class Assign : public Rewriter {
public:
    Assign(World& world, Liveness::Class cls, const Liveness& live, const DefMap<size_t>& regs)
        : Rewriter(world)
        , cls_(cls)
        , live_(live)
        , regs_(regs) {}

    const Def* rewrite(const Def* def) final {
        auto& w = world();
        if (def->isa_mut() || def->is_closed()) return def;
        if (Axm::isa<x86::in>(def) || Axm::isa<x86::fin>(def)) {
            auto app = def->as<App>();
            return w.app(rewrite(app->callee()), raw(app->arg()));
        }
        if (live_.local(def)) {
            if (auto r = fe::lookup(regs_, def))
                return cls_ == Liveness::Gpr ? w.call<x86::in>(w.lit_idx(Num_Regs, *r), raw(def))
                                             : w.call<x86::fin>(w.lit_idx(Num_Fpr, *r), raw(def));
            // A tuple of values, e.g. the whole variable of a block or an instruction, is used through its projections.
            if (auto projs = def->projs(); projs.size() > 1 && std::ranges::any_of(projs, [&](const Def* d) {
                                               return regs_.contains(d) || d->num_projs() > 1;
                                           })) {
                auto ops = DefVec();
                for (auto d : projs)
                    ops.emplace_back(rewrite(d));
                return w.tuple(ops);
            }
            return raw(def);
        }
        return Rewriter::rewrite(def);
    }

private:
    /// @p def, a parameter, an instruction, or a projection thereof, without wrapping it.
    const Def* raw(const Def* def) {
        if (auto ex = def->isa<Extract>()) return world().extract(raw(ex->tuple()), ex->index());
        if (def->isa<Var>()) return def;
        return Rewriter::rewrite(def);
    }

    Liveness::Class cls_;
    const Liveness& live_;
    const DefMap<size_t>& regs_;
};

class Colorer {
public:
    Colorer(Lam* fun, Liveness::Class cls, std::ostream* dot = nullptr)
        : fun_(fun)
        , live_(fun)
        , cls_(cls)
        , dot_(dot)
        , base_(base_costs(cls)) {}

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

    void add(const Def* v);
    pbqp::Costs* costs(const Def* v) {
        auto id = fe::lookup(ids_, strip(v));
        return id ? &costs_[*id] : nullptr;
    }
    void fix(const Def* v, size_t r) {
        if (auto cs = costs(v))
            for (size_t o = 0; o != Num_Regs; ++o)
                if (o != r) (*cs)[o] = pbqp::Cost::inf();
    }
    void forbid(const Def* v, size_t r) {
        if (auto cs = costs(v)) (*cs)[r] = pbqp::Cost::inf();
    }
    void prefer(const Def* v, size_t r) {
        if (auto cs = costs(v))
            for (size_t o = 0; o != Num_Regs; ++o)
                if (o != r) (*cs)[o] += Move;
    }
    void link(const Def* a, const Def* b, pbqp::MatrixView m) {
        a = strip(a), b = strip(b);
        if (a == b) return;
        if (auto i = fe::lookup(ids_, a))
            if (auto j = fe::lookup(ids_, b)) graph_.add_edge_costs(*i, *j, m);
    }
    void jump(Lam* target, const Def* arg) {
        auto n = target->num_vars();
        for (size_t i = 0; i != n; ++i)
            link(arg->proj(n, i), target->var(n, i), move_);
    }

    Lam* fun_;
    Liveness live_;
    Liveness::Class cls_;
    std::ostream* dot_;
    pbqp::Graph graph_;
    DefMap<pbqp::NodeId> ids_;
    DefVec values_;
    fe::Vector<pbqp::Costs> costs_;
    const pbqp::Matrix interfere_
        = pbqp::Matrix(Num_Regs, Num_Regs, [](size_t i, size_t j) { return i == j ? pbqp::Cost::inf() : 0; });
    const pbqp::Matrix tie_
        = pbqp::Matrix(Num_Regs, Num_Regs, [](size_t i, size_t j) { return i == j ? 0 : pbqp::Cost::inf(); });
    const pbqp::Matrix move_ = pbqp::Matrix(Num_Regs, Num_Regs, [](size_t i, size_t j) { return i == j ? 0 : Move; });
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
            if (is_value(x, cls_) && used.contains(x)) add(x);
        for (auto insn : block.insns) {
            auto implied = DefSet();
            for (auto [j, _] : constraint(insn).results)
                implied.emplace(result(insn, j));
            for (auto d : live_.defs(insn))
                if (is_value(d, cls_) && (used.contains(d) || implied.contains(d))) add(d);
        }
    }
}

void Colorer::constraints() {
    for (const auto& block : live_.blocks()) {
        for (auto insn : block.insns) {
            auto c = constraint(insn);
            // Fixed operands, results, and clobbers of an instruction are general-purpose registers.
            if (cls_ == Liveness::Gpr) {
                for (auto [j, r] : c.fixed)
                    fix(c.ops[j], phys(r));
                for (auto [j, r] : c.results)
                    fix(result(insn, j), phys(r));
                for (auto r : c.clobbers)
                    for (auto u : live_.uses(insn))
                        forbid(u, phys(r));
            }
            if (c.tied && is_value(c.ops[*c.tied], cls_)) link(c.ops[*c.tied], result(insn, 0), tie_);
        }
    }
}

void Colorer::copies() {
    auto n = fun_->num_vars();
    for (size_t i = 0, a = 0; i + 1 < n; ++i)
        if (auto x = fun_->var(n, i); is_value(x, cls_) && a < num_arg_regs(cls_)) prefer(x, arg_reg(cls_, a++));

    for (const auto& block : live_.blocks()) {
        auto term   = block.lam->body()->as<App>();
        auto callee = term->callee();
        if (callee == fun_->ret_var()) {
            for (auto a : term->args())
                if (is_value(a, cls_)) prefer(a, ret_reg(cls_));
        } else if (auto ex = callee->isa<Extract>()) {
            for (auto target : ex->tuple()->projs())
                if (auto bb = Lam::isa_mut_basicblock(target); bb && live_.block(bb)) jump(bb, term->arg());
        } else if (auto bb = Lam::isa_mut_basicblock(callee); bb && live_.block(bb)) {
            jump(bb, term->arg());
        } else if (Lam::isa_mut_returning(callee)) {
            auto m = term->num_args();
            for (size_t i = 0, a = 0; i + 1 < m; ++i)
                if (auto x = term->arg(m, i); is_value(x, cls_) && a < num_arg_regs(cls_))
                    prefer(x, arg_reg(cls_, a++));
            if (auto k = call_cont(live_, fun_, block)) {
                for (auto v : k->live_in)
                    if (auto cs = costs(v))
                        for (size_t r = 0; r != Num_Regs; ++r)
                            if (caller_saved(cls_, r)) (*cs)[r] = pbqp::Cost::inf();
                for (auto x : live_.params(*k))
                    if (is_value(x, cls_)) prefer(x, ret_reg(cls_));
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
        fun_->blame(MIM_X86_BE "cannot assign registers in `{}`; conflicting values: {}", fun_,
                    fe::Join(conflicts, ", "))
            .bail();
    }
    for (size_t i = 0; i != values_.size(); ++i)
        regs_[values_[i]] = sol.choices[i];
}

/// Checks the invariants of an assignment over a fresh Liveness of @p fun.
class Checker {
public:
    Checker(Lam* fun, Liveness::Class cls)
        : fun_(fun)
        , live_(fun)
        , cls_(cls) {}

    void run();

private:
    void walk(const Def* def, const Def* at);
    std::optional<size_t> reg(const Def* v) const {
        if (auto r = fe::lookup(regs_, strip(v))) return *r;
        return std::nullopt;
    }
    [[noreturn]] void fail(const Def* at, std::string msg) {
        at->blame(MIM_X86_BE "register assignment of `{}`: {}", fun_, msg).bail();
    }

    Lam* fun_;
    Liveness live_;
    Liveness::Class cls_;
    DefMap<size_t> regs_;
};

void Checker::walk(const Def* def, const Def* at) {
    if (def->isa_mut() || def->is_closed() || is_mem(def)) return;
    auto wrapped = cls_ == Liveness::Gpr ? Axm::isa<x86::in>(def) : Axm::isa<x86::fin>(def);
    if (wrapped) {
        auto v        = wrapped->arg();
        auto r        = in_reg(def);
        auto [i, ins] = regs_.emplace(v, r);
        if (!live_.local(v) || !is_value(v, cls_)) fail(at, std::format("`{}` is no value", v));
        if (is_reserved(cls_, r)) fail(at, std::format("`{}` is in a reserved register", v));
        if (!ins && i->second != r) fail(at, std::format("`{}` is in two registers", v));
        return;
    }
    if (live_.local(def)) {
        for (auto d : def->projs())
            if (is_value(d, cls_)) fail(at, std::format("`{}` is used without a register", d));
        return;
    }
    if (auto app = def->isa<App>(); app && prev_mem(app)) fail(at, std::format("`{}` is off the chain", app));
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
        if (auto ex = term->callee()->isa<Extract>(); ex && isa_sched_cc(ex->index())) {
            walk(ex->tuple(), term);
            walk(term->arg(), term);
        } else {
            walk(term, term);
        }
    }

    for (const auto& block : live_.blocks()) {
        for (auto insn : block.insns) {
            auto c = constraint(insn);
            if (cls_ == Liveness::Gpr) {
                for (auto [j, r] : c.results) {
                    auto d = result(insn, j);
                    if (regs_.emplace(d, phys(r)).first->second != phys(r))
                        fail(insn, std::format("`{}` is not in its fixed register", d));
                }
                for (auto [j, r] : c.fixed)
                    if (!Axm::isa<x86::in>(c.ops[j]) || in_reg(c.ops[j]) != phys(r))
                        fail(insn, std::format("operand `{}` is not in its fixed register", c.ops[j]));
                for (auto r : c.clobbers)
                    for (auto u : live_.uses(insn))
                        if (reg(u) == phys(r)) fail(insn, std::format("operand `{}` is in a clobbered register", u));
            }
            if (c.tied && is_value(c.ops[*c.tied], cls_) && reg(c.ops[*c.tied]) != reg(result(insn, 0)))
                fail(insn, std::format("`{}` is not in the register of operand `{}`", result(insn, 0), c.ops[*c.tied]));
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
                if (auto r = reg(v); r && caller_saved(cls_, *r))
                    fail(block.lam, std::format("`{}` lives across a call in a caller-saved register", v));
    }
}

} // namespace

void Color::visit(Lam* fun) {
    if (!Lam::isa_mut_returning(fun)) return;

    prune(fun);
    Colorer(fun, Liveness::Gpr, dot()).run();
    Colorer(fun, Liveness::Fpr, dot()).run();
    Checker(fun, Liveness::Gpr).run();
    Checker(fun, Liveness::Fpr).run();
}

std::ostream* Color::dot() {
    if (auto d = arg_value(args(), "color-dot")) {
        if (dot_.name().empty()) dot_.name() = std::string(*d);
        return dot_.os();
    }
    return nullptr;
}

} // namespace mim::plug::x86::phase
