#include "mim/plug/x86/phase/spilling.h"

#include <limits>
#include <ranges>

#include <mim/driver.h>

#include "mim/plug/x86/constraint.h"
#include "mim/plug/x86/liveness.h"
#include "mim/plug/x86/subst.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace {

constexpr size_t Inf = std::numeric_limits<size_t>::max();
/// Added to the next-use distance across an edge that leaves a loop.
constexpr size_t Loop_Exit        = 1 << 20;
constexpr size_t Num_Caller_Saved = 9;

size_t add(size_t a, size_t b) { return a > Inf - b ? Inf : a + b; }

DefVec gpr(DefVec values) {
    std::erase_if(values, [](const Def* v) { return !is_gpr(v); });
    return values;
}

const App* root(const Def* value) {
    while (auto ex = value->isa<Extract>())
        value = ex->tuple();
    return value->isa<App>();
}

/// @p value with its instruction @p old_app replaced by @p new_app.
const Def* reroot(const Def* value, const Def* old_app, const Def* new_app) {
    if (value == old_app) return new_app;
    auto ex = value->as<Extract>();
    return value->world().extract(reroot(ex->tuple(), old_app, new_app), ex->index());
}

/// A closed instruction is recomputed instead of reloaded.
bool is_remat(const Def* value) {
    if (auto app = root(value); app && Axm::isa<sched>(app))
        return sched_insn(app)->is_closed() && app->arg()->is_closed();
    return false;
}

class Spiller {
public:
    Spiller(Lam* fun, size_t k)
        : world_(fun->world())
        , fun_(fun)
        , live_(fun)
        , k_(k)
        , n_(live_.blocks().size()) {}

    void run() {
        w_entry_.resize(n_);
        w_exit_.resize(n_);
        for (size_t b = 0; b != n_; ++b)
            reloads_.emplace_back(size(b) + 1);
        cfg();
        next_uses();
        for (auto b : rpo_)
            walk(b);
        couple();
        versions();
        phis();
        build();
    }

private:
    enum class Term { Ret, Call, Branch, Jump };

    struct Reload {
        const Def* value;
        size_t ver;
    };

    const Liveness::Block& block(size_t b) const { return live_.blocks()[b]; }
    size_t size(size_t b) const { return block(b).insns.size(); }
    const App* term(size_t b) const { return block(b).lam->body()->as<App>(); }
    size_t index(Lam* lam) const { return size_t(live_.block(lam) - live_.blocks().data()); }
    Term kind(size_t b) const;
    /// The argument that @p p passes for the @p i-th parameter of @p b, or `nullptr` for a return continuation.
    const Def* arg(size_t p, size_t b, size_t i) const;

    void cfg();
    void next_uses();
    size_t next_use(size_t b, const Def* v, size_t pos) const;
    void entry(size_t b);
    void walk(size_t b);
    void limit(size_t b, DefSet& w, size_t m, size_t pos) const;
    void reload(size_t b, size_t pos, const Def* v);
    void couple();
    void versions();
    void phis();
    size_t new_ver() { return parent_.emplace_back(parent_.size()), phi_.emplace_back(), parent_.size() - 1; }
    size_t find(size_t ver);
    size_t read_entry(const Def* v, size_t b);
    size_t read_exit(const Def* v, size_t b);
    size_t try_remove(size_t phi);
    void build();
    void build(size_t b);
    const Def* sched(const Def* f, const Def* x);
    const Def* spill(const Def* x);

    World& world_;
    Lam* fun_;
    Liveness live_;
    size_t k_;
    size_t n_;

    fe::Vector<size_t> rpo_, rpo_pos_, idom_, depth_;
    fe::Vector<fe::Vector<size_t>> loop_; ///< The body of each loop header; empty otherwise.
    fe::Vector<bool> ret_cont_, branch_target_, done_;
    fe::Vector<DefMap<fe::Vector<size_t>>> uses_;
    fe::Vector<DefMap<size_t>> nu_out_;

    fe::Vector<DefSet> w_entry_, w_exit_;
    fe::Vector<fe::Vector<fe::Vector<Reload>>> reloads_; ///< Per block and position; the terminator is at `size(b)`.
    DefSet spill_;                                       ///< Spilled right after their definition.
    DefSet mphi_;                                        ///< Parameters that become memory φs.

    fe::Vector<size_t> parent_;
    struct Phi {
        size_t b = Inf;
        const Def* value;
        fe::Vector<size_t> ops, users;
    };
    fe::Vector<Phi> phi_;
    DefMap<size_t> def_ver_;
    fe::Vector<DefMap<size_t>> last_, entry_;
    fe::Vector<fe::Vector<size_t>> phis_; ///< The non-trivial φs of each block.

    LamMap<Lam*> new_lams_;
    fe::Vector<const Def*> vers_;
    DefMap<const Def*> stack_; ///< The spill slot of a value.
    const Def* m_ = nullptr;
};

Spiller::Term Spiller::kind(size_t b) const {
    auto callee = term(b)->callee();
    if (callee == fun_->ret_var()) return Term::Ret;
    if (callee->isa<Extract>()) return Term::Branch;
    if (auto lam = Lam::isa_mut_basicblock(callee); lam && live_.block(lam)) return Term::Jump;
    return Term::Call;
}

const Def* Spiller::arg(size_t p, size_t b, size_t i) const {
    if (ret_cont_[b]) return nullptr;
    auto lam = block(b).lam;
    return term(p)->arg(lam->num_vars(), i);
}

void Spiller::cfg() {
    rpo_pos_.resize(n_, Inf);
    idom_.resize(n_, Inf);
    depth_.resize(n_, 0);
    loop_.resize(n_);
    ret_cont_.resize(n_, false);
    branch_target_.resize(n_, false);
    done_.resize(n_, false);

    auto post  = fe::Vector<size_t>();
    auto seen  = fe::Vector<bool>(n_, false);
    auto stack = fe::Vector<std::pair<size_t, size_t>>{{0, 0}};
    seen[0]    = true;
    while (!stack.empty()) {
        auto& [b, i] = stack.back();
        if (i < block(b).succs.size()) {
            auto s = block(b).succs[i++];
            if (!seen[s]) seen[s] = true, stack.emplace_back(s, 0);
        } else {
            post.emplace_back(b);
            stack.pop_back();
        }
    }
    rpo_.assign(post.rbegin(), post.rend());
    for (size_t i = 0; i != rpo_.size(); ++i)
        rpo_pos_[rpo_[i]] = i;

    // Cooper, Harvey, Kennedy: A Simple, Fast Dominance Algorithm.
    auto intersect = [&](size_t a, size_t b) {
        while (a != b) {
            while (rpo_pos_[a] > rpo_pos_[b])
                a = idom_[a];
            while (rpo_pos_[b] > rpo_pos_[a])
                b = idom_[b];
        }
        return a;
    };
    idom_[0] = 0;
    for (bool todo = true; todo;) {
        todo = false;
        for (auto b : rpo_ | std::views::drop(1)) {
            auto dom = Inf;
            for (auto p : block(b).preds)
                if (idom_[p] != Inf) dom = dom == Inf ? p : intersect(p, dom);
            if (idom_[b] != dom) idom_[b] = dom, todo = true;
        }
    }

    auto dominates = [&](size_t a, size_t b) {
        for (;; b = idom_[b]) {
            if (a == b) return true;
            if (b == 0) return false;
        }
    };
    for (size_t b = 0; b != n_; ++b) {
        for (auto h : block(b).succs) {
            if (!dominates(h, b)) continue;
            auto& body = loop_[h];
            auto in    = [&](size_t x) { return std::ranges::contains(body, x); };
            if (!in(h)) body.emplace_back(h);
            for (auto todo = fe::Vector<size_t>{b}; !todo.empty();) {
                auto x = todo.back();
                todo.pop_back();
                if (in(x)) continue;
                body.emplace_back(x);
                for (auto p : block(x).preds)
                    todo.emplace_back(p);
            }
        }
    }
    for (const auto& body : loop_)
        for (auto x : body)
            ++depth_[x];

    for (size_t b = 0; b != n_; ++b) {
        if (kind(b) == Term::Call)
            for (auto a : term(b)->args())
                if (auto lam = Lam::isa_mut_basicblock(a); lam && live_.block(lam)) ret_cont_[index(lam)] = true;
        if (kind(b) == Term::Branch)
            for (auto s : block(b).succs)
                branch_target_[s] = true;
    }
}

void Spiller::next_uses() {
    uses_.resize(n_);
    nu_out_.resize(n_);
    for (size_t b = 0; b != n_; ++b) {
        for (size_t i = 0; i != size(b); ++i)
            for (auto v : gpr(live_.uses(block(b).insns[i])))
                uses_[b][v].emplace_back(i);
        for (auto v : gpr(live_.term_uses(block(b))))
            uses_[b][v].emplace_back(size(b));
    }

    for (bool todo = true; todo;) {
        todo = false;
        for (auto b : rpo_ | std::views::reverse) {
            for (auto v : block(b).live_out) {
                if (!is_gpr(v)) continue;
                auto d = Inf;
                for (auto s : block(b).succs)
                    if (block(s).live_in.contains(v))
                        d = std::min(d, add(next_use(s, v, 0), depth_[s] < depth_[b] ? Loop_Exit : 0));
                auto [i, ins] = nu_out_[b].emplace(v, d);
                if (!ins && d < i->second) i->second = d, todo = true;
                if (ins && d != Inf) todo = true;
            }
        }
    }
}

size_t Spiller::next_use(size_t b, const Def* v, size_t pos) const {
    if (auto us = fe::lookup(uses_[b], v))
        if (auto i = std::ranges::lower_bound(*us, pos); i != us->end()) return *i - pos;
    if (auto d = fe::lookup(nu_out_[b], v)) return add(size(b) + 1 - pos, *d);
    return Inf;
}

void Spiller::limit(size_t b, DefSet& w, size_t m, size_t pos) const {
    while (w.size() > m) {
        auto victim
            = *std::ranges::max_element(w, {}, [&](const Def* v) { return std::pair(next_use(b, v, pos), v->gid()); });
        w.erase(victim);
    }
}

void Spiller::reload(size_t b, size_t pos, const Def* v) {
    auto& rs = reloads_[b][pos];
    if (std::ranges::contains(rs, v, &Reload::value)) return;
    rs.emplace_back(Reload{v, Inf});
    if (!mphi_.contains(v) && !is_remat(v)) spill_.emplace(v);
}

void Spiller::entry(size_t b) {
    auto lam    = block(b).lam;
    auto n      = lam->num_vars();
    auto params = DefVec();
    for (size_t i = 0; i != n; ++i)
        if (auto x = lam->var(n, i); is_gpr(x)) params.emplace_back(x);

    auto take   = DefVec();
    auto by_use = [&](DefVec& vs) {
        std::ranges::sort(vs, {}, [&](const Def* v) { return std::pair(next_use(b, v, 0), v->gid()); });
    };
    auto fill = [&](DefVec vs, size_t cap) {
        by_use(vs);
        for (auto v : vs)
            if (take.size() < cap && next_use(b, v, 0) != Inf && !std::ranges::contains(take, v)) take.emplace_back(v);
    };

    auto vars  = lam->vars();
    auto cands = DefVec();
    for (auto v : block(b).live_in)
        if (is_gpr(v)) cands.emplace_back(v);

    // These parameters cannot become memory φs; a dead one leaves its register right away.
    auto fixed = b == 0 || ret_cont_[b] || branch_target_[b];
    for (auto x : params)
        if (fixed || next_use(b, x, 0) == Inf) take.emplace_back(x);
    if (take.size() > k_) lam->blame(MIM_X86_BE "`{}` takes more than {} register arguments", lam, k_).bail();
    std::erase_if(params, [&](const Def* x) { return std::ranges::contains(take, x); });
    std::ranges::copy(params, std::back_inserter(cands));

    auto preds = block(b).preds;
    if (std::ranges::all_of(preds, [&](size_t p) { return done_[p]; })) {
        auto avail = DefMap<size_t>();
        for (auto p : preds) {
            for (auto v : cands) {
                auto i = size_t(std::ranges::find(vars, v) - vars.begin());
                auto a = i < n ? arg(p, b, i) : v;
                if (!a || Lit::isa(a) || w_exit_[p].contains(a)) ++avail[v];
            }
        }
        auto all = DefVec(), some = DefVec();
        for (auto v : cands)
            if (auto i = avail.find(v); i != avail.end()) (i->second == preds.size() ? all : some).emplace_back(v);
        fill(all, k_);
        fill(some, k_);
    } else {
        auto in_loop = DefVec(), through = DefVec();
        for (auto v : cands)
            (next_use(b, v, 0) < Loop_Exit ? in_loop : through).emplace_back(v);
        fill(in_loop, k_);
        auto pressure = size_t(0);
        for (auto x : loop_[b])
            pressure = std::max(pressure, block(x).max_live[Liveness::Gpr]);
        auto busy = pressure > through.size() ? pressure - through.size() : 0;
        fill(through, std::min(k_, take.size() + (k_ > busy ? k_ - busy : 0)));
    }

    w_entry_[b] = DefSet(take.begin(), take.end());
    for (auto x : params)
        if (!w_entry_[b].contains(x)) mphi_.emplace(x);
}

void Spiller::walk(size_t b) {
    entry(b);
    auto w = w_entry_[b];
    auto n = size(b);

    auto use = [&](const DefVec& us, size_t pos, size_t more, const Def* at) {
        if (us.size() + more > k_) at->blame(MIM_X86_BE "`{}` needs more than {} registers", at, k_).bail();
        for (auto v : us)
            if (w.emplace(v).second) reload(b, pos, v);
        limit(b, w, k_ - more, pos);
    };

    for (size_t i = 0; i != n; ++i) {
        auto insn = block(b).insns[i];
        auto us   = gpr(live_.uses(insn));
        auto c    = constraint(insn);
        auto live = [&](const Def* v) { return next_use(b, v, i + 1) != Inf; };
        auto cp   = DefVec();
        for (auto [j, _] : copies(c, live))
            if (live(c.ops[j])) cp.emplace_back(c.ops[j]);
        // If the copies of live operands do not fit, these operands leave their registers at the instruction instead.
        auto evict = us.size() + c.clobbers.size() + cp.size() > k_;
        use(us, i, c.clobbers.size() + (evict ? 0 : cp.size()), insn);
        if (evict)
            for (auto v : cp)
                w.erase(v);
        auto ds = gpr(live_.defs(insn));
        if (is_sched_cc(insn)) std::erase_if(ds, [&](const Def* d) { return next_use(b, d, i + 1) == Inf; });
        if (ds.size() > k_) insn->blame(MIM_X86_BE "`{}` defines more than {} registers", insn, k_).bail();
        limit(b, w, k_ - ds.size(), i + 1);
        w.insert(ds.begin(), ds.end());
    }

    auto t = kind(b);
    if (t != Term::Jump) use(gpr(live_.term_uses(block(b))), n, 0, term(b));
    if (t == Term::Call) limit(b, w, k_ > Num_Caller_Saved ? k_ - Num_Caller_Saved : 0, n + 1);
    w_exit_[b] = std::move(w);
    done_[b]   = true;
}

void Spiller::couple() {
    for (size_t b = 0; b != n_; ++b) {
        auto lam = block(b).lam;
        auto n   = lam->num_vars();
        for (auto p : block(b).preds) {
            auto end  = size(p);
            auto need = [&](const Def* v) {
                if (w_exit_[p].contains(v)) return;
                if (block(p).succs.size() != 1)
                    lam->blame(MIM_X86_BE "critical edge from `{}` to `{}`", block(p).lam, lam).bail();
                reload(p, end, v);
            };

            for (auto v : w_entry_[b])
                if (block(b).live_in.contains(v)) need(v);

            for (size_t i = 0; i != n; ++i) {
                auto x = lam->var(n, i);
                auto a = arg(p, b, i);
                if (!a || !is_gpr(x) || Lit::isa(a)) continue;
                if (!w_entry_[b].contains(x)) {
                    if (!mphi_.contains(a)) spill_.emplace(a);
                } else if (next_use(b, x, 0) != Inf) {
                    need(a);
                }
            }
        }
    }
}

void Spiller::versions() {
    last_.resize(n_);
    for (auto b : rpo_) {
        auto lam = block(b).lam;
        for (auto x : lam->vars())
            if (w_entry_[b].contains(x)) def_ver_[x] = new_ver();
        auto n = size(b);
        for (size_t i = 0; i <= n; ++i) {
            for (auto& r : reloads_[b][i])
                last_[b][r.value] = r.ver = new_ver();
            if (i == n) break;
            for (auto d : gpr(live_.defs(block(b).insns[i])))
                last_[b][d] = def_ver_[d] = new_ver();
        }
    }
}

size_t Spiller::find(size_t ver) {
    while (parent_[ver] != ver)
        ver = parent_[ver] = parent_[parent_[ver]];
    return ver;
}

size_t Spiller::read_exit(const Def* v, size_t b) {
    if (auto ver = fe::lookup(last_[b], v)) return *ver;
    return read_entry(v, b);
}

size_t Spiller::read_entry(const Def* v, size_t b) {
    if (auto ver = fe::lookup(entry_[b], v)) return *ver;
    if (auto ver = fe::lookup(def_ver_, v); ver && &live_.def_block(v) == &block(b)) return *ver;

    const auto& preds = block(b).preds;
    if (preds.empty()) v->blame(MIM_X86_BE "`{}` is not in a register at the entry", v).bail();
    if (preds.size() == 1) return entry_[b][v] = read_exit(v, preds.front());

    // Braun et al., Simple and Efficient Construction of Static Single Assignment Form, CC 2013.
    auto phi        = new_ver();
    phi_[phi].b     = b;
    phi_[phi].value = v;
    entry_[b][v]    = phi;
    for (auto p : preds) {
        auto op = read_exit(v, p);
        phi_[phi].ops.emplace_back(op);
        phi_[op].users.emplace_back(phi);
    }
    return entry_[b][v] = try_remove(phi);
}

size_t Spiller::try_remove(size_t phi) {
    auto same = Inf;
    for (auto op : phi_[phi].ops) {
        op = find(op);
        if (op == same || op == phi) continue;
        if (same != Inf) return phi;
        same = op;
    }
    assert(same != Inf);
    parent_[phi] = same;
    auto users   = std::move(phi_[phi].users);
    for (auto u : users)
        phi_[same].users.emplace_back(u);
    for (auto u : users)
        if (u != phi && find(u) == u) try_remove(u);
    return same;
}

void Spiller::phis() {
    entry_.resize(n_);
    phis_.resize(n_);
    for (auto b : rpo_)
        for (auto v : w_entry_[b])
            if (block(b).live_in.contains(v)) read_entry(v, b);
    for (size_t ver = 0; ver != phi_.size(); ++ver)
        if (phi_[ver].b != Inf && find(ver) == ver) phis_[phi_[ver].b].emplace_back(ver);
}

const Def* Spiller::sched(const Def* f, const Def* x) {
    auto res = world_.call<x86::sched>(f, m_, x);
    m_       = res->proj(2, 0);
    return res->proj(2, 1);
}

const Def* Spiller::spill(const Def* x) { return sched(world_.app(world_.annex<x86::spill>(), Idx::as(x->type())), x); }

void Spiller::build() {
    vers_.resize(parent_.size());
    for (size_t b = 1; b != n_; ++b) {
        auto lam  = block(b).lam;
        auto n    = lam->num_vars();
        auto mems = std::ranges::count_if(lam->vars(), [&](const Def* x) { return mphi_.contains(x); });
        if (mems == 0 && phis_[b].empty()) continue;
        if (ret_cont_[b]) lam->blame(MIM_X86_BE "return continuation `{}` would need new parameters", lam).bail();

        auto dom = DefVec();
        for (size_t i = 0; i != n; ++i) {
            auto x = lam->var(n, i);
            dom.emplace_back(mphi_.contains(x) ? world_.call<Spill>(Idx::as(x->type())) : x->type());
        }
        for (auto phi : phis_[b])
            dom.emplace_back(phi_[phi].value->type());
        auto new_lam = world_.mut_con(dom);
        new_lam->set(lam->dbg());
        auto m = dom.size();
        for (size_t i = 0; i != n; ++i)
            new_lam->var(m, i)->set(lam->var(n, i)->sym());
        for (size_t j = 0; j != phis_[b].size(); ++j)
            new_lam->var(m, n + j)->set(phi_[phis_[b][j]].value->sym());
        new_lams_[lam] = new_lam;
    }

    for (auto b : rpo_)
        build(b);
}

void Spiller::build(size_t b) {
    auto old     = block(b).lam;
    auto new_lam = fe::lookup(new_lams_, old);
    if (!new_lam) new_lam = old;
    auto n   = old->num_vars();
    auto m   = new_lam->num_vars();
    auto cur = Def2Def();
    m_       = nullptr;

    for (size_t i = 0; i != n; ++i) {
        auto x = old->var(n, i);
        auto y = new_lam->var(m, i);
        if (is_mem(x) && !m_) m_ = y;
        if (mphi_.contains(x))
            stack_[x] = y;
        else
            cur[x] = y;
        if (auto ver = fe::lookup(def_ver_, x)) vers_[*ver] = y;
    }
    for (size_t j = 0; j != phis_[b].size(); ++j)
        vers_[phis_[b][j]] = new_lam->var(m, n + j);
    for (const auto& [v, ver] : entry_[b])
        cur[v] = vers_[find(ver)];
    for (auto x : old->vars())
        if (spill_.contains(x)) stack_[x] = spill(cur[x]);

    auto subst   = [&](const Def* def) { return Subst(world_, cur, m_, &new_lams_).rewrite(def); };
    auto reloads = [&](size_t pos) {
        for (auto [v, ver] : reloads_[b][pos]) {
            const Def* val;
            if (is_remat(v)) {
                auto app = root(v);
                auto res = world_.call<x86::sched>(sched_insn(app), m_, app->arg());
                m_       = res->proj(2, 0);
                val      = reroot(v, app, res);
            } else {
                auto slot = fe::lookup(stack_, v);
                if (!slot) v->blame(MIM_X86_BE "`{}` has no spill slot", v).bail();
                val = sched(world_.app(world_.annex<x86::reload>(), Idx::as(v->type())), slot);
            }
            cur[v] = vers_[ver] = val;
        }
    };

    for (size_t i = 0, e = size(b); i != e; ++i) {
        reloads(i);
        auto insn    = block(b).insns[i];
        auto new_app = Subst(world_, cur, m_, &new_lams_).Rewriter::rewrite(insn);
        m_           = is_mem(new_app) ? new_app : new_app->proj(2, 0);
        cur[insn]    = new_app;
        for (auto d : live_.defs(insn)) {
            cur[d] = reroot(d, insn, new_app);
            if (auto ver = fe::lookup(def_ver_, d)) vers_[*ver] = cur[d];
            if (spill_.contains(d)) stack_[d] = spill(cur[d]);
        }
    }
    reloads(size(b));

    auto t    = term(b);
    auto body = static_cast<const Def*>(nullptr);
    if (kind(b) == Term::Jump) {
        auto target = t->callee()->as_mut<Lam>();
        auto s      = index(target);
        auto k      = target->num_vars();
        auto args   = DefVec();
        for (size_t i = 0; i != k; ++i) {
            auto x = target->var(k, i);
            auto a = t->arg(k, i);
            if (!mphi_.contains(x)) {
                // A dead parameter may receive anything.
                auto dead = is_gpr(a) && !Lit::isa(a) && !cur.contains(a);
                args.emplace_back(dead ? world_.lit(x->type(), 0) : subst(a));
            } else if (Lit::isa(a)) {
                args.emplace_back(spill(a));
            } else {
                auto slot = fe::lookup(stack_, a);
                if (!slot) a->blame(MIM_X86_BE "`{}` has no spill slot", a).bail();
                args.emplace_back(slot);
            }
        }
        auto p = size_t(std::ranges::find(block(s).preds, b) - block(s).preds.begin());
        for (auto phi : phis_[s])
            args.emplace_back(vers_[find(phi_[phi].ops[p])]);
        // m_ may have moved on through the spills of literals.
        for (auto& a : args)
            if (is_mem(a)) a = m_;
        body = world_.app(subst(target), world_.tuple(args));
    } else {
        if (kind(b) == Term::Branch)
            for (auto target : t->callee()->as<Extract>()->tuple()->projs())
                if (auto lam = target->isa_mut<Lam>(); lam && new_lams_.contains(lam))
                    lam->blame(MIM_X86_BE "branch target `{}` would need new parameters", lam).bail();
        body = subst(t);
    }

    auto filter = old->filter();
    if (new_lam == old)
        old->unset()->set(subst(filter), body);
    else
        new_lam->set(filter->is_closed() ? filter : world_.lit_ff(), body);
}

/// A hash-consed branch tuple may share one target among several branches; each branch then gets its own wrapper.
void split_critical_edges(Lam* fun) {
    auto& w     = fun->world();
    auto preds  = LamMap<size_t>();
    auto blocks = fe::Vector<Lam*>{fun};
    auto seen   = LamSet{fun};
    auto succ   = [&](const Def* def) {
        if (auto bb = Lam::isa_mut_basicblock(def); bb && bb->is_set()) {
            ++preds[bb];
            if (seen.emplace(bb).second) blocks.emplace_back(bb);
        }
    };
    for (size_t i = 0; i != blocks.size(); ++i) {
        auto term = blocks[i]->body()->isa<App>();
        if (!term) continue;
        succ(term->callee());
        if (auto ex = term->callee()->isa<Extract>())
            for (auto target : ex->tuple()->projs())
                succ(target);
        for (auto arg : term->args())
            succ(arg);
    }

    for (auto lam : blocks) {
        auto term = lam->body()->isa<App>();
        auto ex   = term ? term->callee()->isa<Extract>() : nullptr;
        if (!ex) continue;
        auto split   = false;
        auto targets = ex->tuple()->projs();
        for (auto& target : targets) {
            auto bb = Lam::isa_mut_basicblock(target);
            if (!bb || preds[bb] < 2) continue;
            split        = true;
            auto wrapper = w.mut_con(bb->dom());
            wrapper->set(bb->dbg());
            target = wrapper->set(w.lit_ff(), w.app(bb, wrapper->var()));
            w.log().d("split critical edge from `{}` to `{}` via `{}`", lam, bb, wrapper);
        }
        if (split) {
            auto filter = lam->filter();
            lam->unset()->set(filter, w.app(w.extract(w.tuple(targets), ex->index()), term->arg()));
        }
    }
}

} // namespace

Spilling::Spilling(World& world, flags_t annex)
    : ClosedMutPhase(world, annex, true)
    , k_(driver().arg_nat("x86", "k").value_or(14)) {
    if (k_ == 0) {
        log().w("ignoring `-X x86:k=0`");
        k_ = 14;
    }
}

void Spilling::visit(Lam* fun) {
    if (Lam::isa_mut_returning(fun)) {
        split_critical_edges(fun);
        Spiller(fun, k_).run();
    }
}

} // namespace mim::plug::x86::phase
