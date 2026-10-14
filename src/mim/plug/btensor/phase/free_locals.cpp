#include "mim/plug/btensor/phase/free_locals.h"

#include <algorithm>
#include <deque>
#include <optional>
#include <ranges>

#include <mim/axm.h>
#include <mim/lam.h>
#include <mim/nest.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/btensor/btensor.h"

namespace mim::plug::btensor::phase {

namespace {

/// Is @p app an op whose buffer result is freshly allocated?
bool allocates(const App* app) {
    return Axm::isa<buffer::alloc>(app) || Axm::isa<buffer::init>(app) || Axm::isa<buffer::lit>(app);
}

/// The buffer @p app writes into and returns: `buffer.write`'s operand or a btensor op's `dst`; `nullptr` otherwise.
const Def* written(const App* app) {
    if (Axm::isa<buffer::write>(app) || Axm::isa<btensor::map_reduce>(app) || Axm::isa<btensor::map_reduce_post>(app)
        || Axm::isa<btensor::broadcast>(app) || Axm::isa<btensor::pad>(app) || Axm::isa<btensor::concat>(app)
        || Axm::isa<btensor::gather>(app) || Axm::isa<btensor::scatter>(app))
        return app->arg()->proj(app->arg()->num_projs(), 1);
    return nullptr;
}

/// The allocation @p d denotes, seen through the ops that return the buffer they wrote; `nullptr` if it denotes none.
const App* alloc_of(const Def* d) {
    for (;;) {
        auto ex = d->isa<Extract>();
        if (!ex) return nullptr;
        auto app = ex->tuple()->isa<App>();
        auto i   = Lit::isa<u64>(ex->index());
        if (!app || !i || *i != 1) return nullptr;
        if (allocates(app)) return app;
        d = written(app);
        if (!d) return nullptr;
    }
}

/// The Lam a - possibly projected - parameter belongs to; `nullptr` for anything else.
Lam* param_of(const Def* d) {
    while (auto ex = d->isa<Extract>())
        d = ex->tuple();
    if (auto var = d->isa<Var>()) return var->binder()->isa_mut<Lam>();
    return nullptr;
}

/// Peels identity annotations such as `ll.vec` off a loop body.
const Def* unwrap(const Def* d) {
    while (auto app = d->isa<App>()) {
        if (!app->axm() || app->type() != app->arg()->type()) break;
        d = app->arg();
    }
    return d;
}

/// Can a value of type @p t hold a buffer - directly, inside an aggregate, or captured by a closure?
bool may_hold_buf(const Def* t) {
    if (!t) return false;
    if (Axm::isa<buffer::Buf>(t) || t->isa<Pi>()) return true;
    if (auto sigma = t->isa<Sigma>()) return std::ranges::any_of(sigma->ops(), may_hold_buf);
    if (auto arr = t->isa<Arr>()) return may_hold_buf(arr->body());
    return false;
}

/// The index of @p lam's memory parameter; `std::nullopt` unless it is a top-level parameter.
std::optional<size_t> mem_index(Lam* lam) {
    auto n = lam->num_vars();
    for (size_t i = 0; i != n; ++i)
        if (mem::isa_mem(lam->var(n, i))) return i;
    return std::nullopt;
}

DefVec sorted(const DefSet& set) {
    auto vec = DefVec();
    for (auto def : set)
        vec.push_back(def);
    std::ranges::sort(vec, GIDLt<const Def*>());
    return vec;
}

/// The liveness analysis of one function: the root Lam and the continuations nested in it.
class Scope {
public:
    struct Node {
        Lam* lam;
        const App* tail = nullptr;
        fe::Vector<const App*> ops; ///< The memory chain, in program order.
        DefVec mem_results;         ///< The memory each op yields to its consumer.
        fe::Vector<DefSet> uses;    ///< Per op: the allocations it mentions.
        DefSet defs;                ///< The allocations on this chain.
        fe::Vector<Lam*> succs;     ///< Successors referenced in the tail: trampolines can be spliced in.
        fe::Vector<Lam*> var_succs; ///< Successors reached through a continuation parameter.
        DefVec conts;               ///< Continuation parameters of nested lams the tail hands on; resolved late.
        bool unknown = false;       ///< Anything may follow.
        DefSet live_in, live_out;
    };

    /// The targets a continuation parameter of a nested Lam can stand for.
    struct Targets {
        LamSet lams;
        bool unknown = false;
    };

    Scope(World& world, const Nest& nest, Lam* root)
        : world_(world)
        , nest_(nest)
        , root_(root) {
        collect_members(nest[root]);
    }

    /// @returns `false` if the function's shape defeats the analysis; then nothing is freed in it.
    bool run() {
        discover();
        resolve();
        for (auto& n : nodes_) {
            if (tainted_.contains(n.lam)) continue;
            if (!chain(n)) {
                world_.log().d("free_locals: unexpected memory chain in `{}`", n.lam);
                return false;
            }
            collect_uses(n);
        }
        liveness();
        return true;
    }

    /// @name Results
    ///@{
    DefMap<DefVec> after;
    LamMap<LamMap<DefVec>> edges;
    LamMap<const App*> tails;
    DefSet whole;
    ///@}

private:
    bool member(Lam* lam) const { return members_.contains(lam); }

    void collect_members(const Nest::Node* node) {
        members_.emplace(node->mut());
        for (auto child : node->children().nodes())
            collect_members(child);
    }

    bool nested_in(const Def* alloc, Lam* lam) const {
        auto def_node = nest_[def_node_.find(alloc)->second->lam];
        for (auto node = nest_[lam]; node; node = node->inest())
            if (node == def_node) return true;
        return false;
    }

    Node* node(Lam* lam) {
        if (auto i = lam2node_.find(lam); i != lam2node_.end()) return i->second;
        auto& n = nodes_.emplace_back();
        n.lam   = lam;
        lam2node_.emplace(lam, &n);
        return &n;
    }

    Node* succ(Node* n, Lam* lam, std::deque<Node*>& todo) {
        auto s = node(lam);
        if (std::ranges::find(n->succs, lam) == n->succs.end()) {
            n->succs.push_back(lam);
            todo.push_back(s);
        }
        return s;
    }

    /// @name Discovery
    /// Collects the continuations control can reach from the root, with their syntactic successors.
    ///@{
    void discover() {
        std::deque<Node*> todo;
        todo.push_back(node(root_));

        while (!todo.empty()) {
            auto n = todo.front();
            todo.pop_front();
            auto lam  = n->lam;
            auto tail = lam->is_set() ? lam->body()->isa<App>() : nullptr;
            if (!tail) continue;
            n->tail     = tail;
            auto callee = tail->callee();
            auto arg    = tail->arg();

            if (auto binder = param_of(callee)) {
                // Via the root's own continuation parameter control leaves the function.
                if (binder != root_) {
                    if (member(binder))
                        n->conts.push_back(callee);
                    else
                        n->unknown = true;
                }
                classify_args(n, arg, nullptr, todo);
            } else if (auto k = callee->isa_mut<Lam>()) {
                if (member(k)) {
                    succ(n, k, todo);
                    jump(n, k, arg, todo);
                } else {
                    classify_args(n, arg, nullptr, todo);
                }
            } else if (auto for_ax = Axm::isa<affine::For>(tail)) {
                auto [body, exit, _] = for_ax->uncurry_args<3>();
                auto b               = unwrap(body)->isa_mut<Lam>();
                auto e               = exit->isa_mut<Lam>();
                if (b && e && member(b) && member(e) && b->num_vars() == 3) {
                    succ(n, b, todo);
                    succ(n, e, todo);
                    auto& targets = cont_[b->var(3, 2)];
                    targets.lams.emplace(b);
                    targets.lams.emplace(e);
                } else {
                    n->unknown = true;
                }
                classify_args(n, arg, nullptr, todo);
            } else if (auto ex = callee->isa<Extract>()) {
                auto tuple = ex->tuple()->isa<Tuple>();
                if (!tuple) {
                    n->unknown = true;
                    classify_args(n, arg, nullptr, todo);
                    continue;
                }
                for (auto op : tuple->ops()) {
                    auto k = op->isa_mut<Lam>();
                    if (k && member(k)) {
                        succ(n, k, todo);
                        jump(n, k, arg, todo);
                    } else {
                        n->unknown = true;
                    }
                }
            } else if (callee->is_closed()) {
                // A global function or an axiom such as `mem.slot`: it may invoke the continuations handed to it.
                classify_args(n, arg, nullptr, todo);
            } else {
                n->unknown = true;
                classify_args(n, arg, nullptr, todo);
            }
        }
    }

    /// Records what the continuation parameters of @p k stand for at this jump.
    void jump(Node* n, Lam* k, const Def* arg, std::deque<Node*>& todo) {
        auto nk    = k->num_vars();
        auto flat  = nk == 1 || arg->num_projs() == nk;
        auto conts = LamSet();
        for (size_t i = 0; i != nk && k != root_; ++i) {
            auto p = k->var(nk, i);
            if (!Pi::isa_cn(p->type())) continue;
            auto& targets = cont_[p];
            auto a        = flat ? arg->proj(nk, i) : nullptr;
            if (auto kk = a ? a->isa_mut<Lam>() : nullptr; kk && member(kk)) {
                targets.lams.emplace(kk);
                conts.emplace(kk);
            } else if (!a || param_of(a) != root_) {
                targets.unknown = true;
            }
        }
        classify_args(n, arg, &conts, todo);
    }

    /// Nested lams reachable through tuples are continuations the callee may invoke: successors.
    /// Anything else leaving in an argument is handed to unknown code: it is tainted.
    /// A continuation parameter of a nested lam that leaves is resolved later.
    void classify_args(Node* n, const Def* d, const LamSet* known, std::deque<Node*>& todo) {
        auto seen = DefSet();
        classify_args(n, d, known, todo, seen, true);
    }

    void
    classify_args(Node* n, const Def* d, const LamSet* known, std::deque<Node*>& todo, DefSet& seen, bool via_tuple) {
        if (d->is_closed() || !may_hold_buf(d->type()) || !seen.emplace(d).second) return;
        if (auto binder = param_of(d)) {
            if (binder != root_ && member(binder) && Pi::isa_cn(d->type())) n->conts.push_back(d);
            return;
        }
        if (auto lam = d->isa_mut<Lam>()) {
            if (!member(lam)) return;
            if (via_tuple || (known && known->contains(lam)))
                succ(n, lam, todo);
            else
                taint(lam);
            return;
        }
        if (d->isa_mut()) return;
        auto ex         = d->isa<Extract>();
        auto structural = d->isa<Tuple>() || (ex && ex->tuple()->isa<Tuple>());
        // A continuation of unknown provenance (e.g. one a direct-style call returned) may lead anywhere.
        if (Pi::isa_cn(d->type()) && !structural) n->unknown = true;
        via_tuple &= structural;
        for (auto op : d->ops())
            if (op) classify_args(n, op, known, todo, seen, via_tuple);
    }

    /// Resolves the continuation parameters the tails jump to or hand on.
    void resolve() {
        for (auto& n : nodes_) {
            for (auto cont : n.conts) {
                auto i = cont_.find(cont);
                if (i == cont_.end() || i->second.unknown) {
                    n.unknown = true;
                    continue;
                }
                for (auto lam : i->second.lams)
                    if (std::ranges::find(n.var_succs, lam) == n.var_succs.end()) n.var_succs.push_back(lam);
            }
        }
    }
    ///@}

    /// @name Mentions
    ///@{
    struct Walk {
        DefSet seen;
        DefSet allocs;
        bool escape        = false; ///< Tail position: nested lams found here are closures handed out.
        const LamSet* skip = nullptr;
        bool unknown_cont  = false; ///< A continuation parameter of a nested lam flows into an op.
    };

    /// Collects the allocations @p d mentions through values; opaque nested lams are entered.
    void walk(const Def* d, Walk& w) {
        // A value whose type cannot hold a buffer (a loaded scalar, say) cannot alias one.
        if (d->is_closed() || !may_hold_buf(d->type()) || !w.seen.emplace(d).second) return;
        if (auto alloc = alloc_of(d)) return (void)w.allocs.emplace(alloc);
        if (auto app = d->isa<App>()) {
            if (allocates(app)) return (void)w.allocs.emplace(app);
            // The result tuple of a write-like op carries its destination, not its inputs.
            if (auto dst = written(app)) return walk(dst, w);
        }
        if (auto binder = param_of(d)) {
            if (!w.escape && binder != root_ && member(binder) && Pi::isa_cn(d->type())) w.unknown_cont = true;
            return;
        }
        if (auto lam = d->isa_mut<Lam>()) {
            if (!member(lam) || (w.skip && w.skip->contains(lam))) return;
            if (w.escape || lam2node_.contains(lam)) return taint(lam);
            for (auto op : lam->ops())
                if (op) walk(op, w);
            return;
        }
        if (d->isa_mut()) return;
        for (auto op : d->ops())
            if (op) walk(op, w);
    }

    /// @p lam leaves the function or is invoked at unknown times: everything it mentions escapes and it hosts no frees.
    void taint(Lam* lam) {
        if (!tainted_.emplace(lam).second) return;
        auto w   = Walk();
        w.escape = true;
        for (auto op : lam->ops())
            if (op) walk(op, w);
        for (auto alloc : w.allocs)
            escaped_.emplace(alloc);
    }
    ///@}

    /// @name Per node
    ///@{
    /// Walks @p n's memory chain back from the tail to its memory parameter.
    bool chain(Node& n) {
        if (!n.tail) return true;
        auto mem_var = mem::mem_var(n.lam);
        auto consume = [this](const Def* arg) {
            auto cur = mem::mem_def(arg);
            if (cur && arg->isa<App>() && !mem::isa_mem(arg)) whole.emplace(arg);
            return cur;
        };
        auto cur = consume(n.tail->arg());
        if (!cur) return true;
        if (!mem_var) return false;
        while (cur != mem_var) {
            auto ex = cur->isa<Extract>();
            auto op = ex ? ex->tuple()->isa<App>() : cur->isa<App>();
            if (!op) return false;
            n.ops.push_back(op);
            n.mem_results.push_back(cur);
            cur = consume(op->arg());
            if (!cur) return false;
        }
        std::ranges::reverse(n.ops);
        std::ranges::reverse(n.mem_results);
        return true;
    }

    void collect_uses(Node& n) {
        n.uses.resize(n.ops.size());
        for (size_t i = 0, e = n.ops.size(); i != e; ++i) {
            auto op = n.ops[i];
            auto w  = Walk{};
            walk(op->callee(), w);
            walk(op->arg(), w);
            if (w.unknown_cont) n.unknown = true;
            if (allocates(op)) {
                w.allocs.emplace(op);
                n.defs.emplace(op);
                def_node_.emplace(op, &n);
            }
            n.uses[i] = std::move(w.allocs);
        }
        if (!n.tail) return;

        auto skip = LamSet(n.succs.begin(), n.succs.end());
        auto w    = Walk();
        w.escape  = true;
        w.skip    = &skip;
        walk(n.tail->callee(), w);
        walk(n.tail->arg(), w);
        for (auto alloc : w.allocs)
            escaped_.emplace(alloc);
    }
    ///@}

    /// @name Liveness
    ///@{
    void liveness() {
        auto owned = DefSet();
        for (auto& n : nodes_)
            for (auto alloc : n.defs)
                if (!escaped_.contains(alloc) && !tainted_.contains(n.lam)) owned.emplace(alloc);
        if (owned.empty()) return;

        auto live = [&](const DefSet& set, DefSet& out) {
            for (auto alloc : set)
                if (owned.contains(alloc)) out.emplace(alloc);
        };
        auto succs = [this](const Node& n) {
            auto res = fe::Vector<const Node*>();
            for (auto lams : {&n.succs, &n.var_succs})
                for (auto lam : *lams)
                    if (!tainted_.contains(lam)) res.push_back(lam2node_.find(lam)->second);
            return res;
        };

        for (bool changed = true; changed;) {
            changed = false;
            for (auto& n : nodes_ | std::views::reverse) {
                if (tainted_.contains(n.lam)) continue;
                auto out = DefSet();
                if (n.unknown) {
                    for (auto& m : nodes_)
                        if (!tainted_.contains(m.lam)) live(m.live_in, out);
                } else {
                    for (auto s : succs(n))
                        live(s->live_in, out);
                }
                // An allocation dominates its uses, so neither makes it live at the entry.
                auto in = DefSet();
                for (auto alloc : out)
                    if (!n.defs.contains(alloc)) in.emplace(alloc);
                for (auto& uses : n.uses)
                    for (auto alloc : uses)
                        if (owned.contains(alloc) && !n.defs.contains(alloc)) in.emplace(alloc);
                if (in != n.live_in || out != n.live_out) {
                    n.live_in  = std::move(in);
                    n.live_out = std::move(out);
                    changed    = true;
                }
            }
        }

        for (auto& n : nodes_) {
            if (tainted_.contains(n.lam)) continue;
            auto last = DefMap<size_t>();
            for (size_t i = 0, e = n.ops.size(); i != e; ++i)
                for (auto alloc : n.uses[i])
                    if (owned.contains(alloc)) last[alloc] = i;
            for (auto [alloc, i] : last)
                if (!n.live_out.contains(alloc)) after[n.mem_results[i]].push_back(alloc);

            if (n.unknown) continue;
            for (auto lam : n.succs) {
                if (tainted_.contains(lam)) continue;
                auto s     = lam2node_.find(lam)->second;
                auto dying = DefSet();
                for (auto alloc : n.live_out)
                    if (!s->live_in.contains(alloc) && nested_in(alloc, n.lam)) dying.emplace(alloc);
                if (!dying.empty()) edges[n.lam][lam] = sorted(dying);
            }
            if (edges.contains(n.lam)) tails[n.lam] = n.tail;
        }
        for (auto& [_, allocs] : after)
            std::ranges::sort(allocs, GIDLt<const Def*>());
    }
    ///@}

    World& world_;
    const Nest& nest_;
    Lam* root_;
    MutSet members_;
    std::deque<Node> nodes_;
    LamMap<Node*> lam2node_;
    DefMap<Targets> cont_;
    LamSet tainted_;
    DefSet escaped_;
    DefMap<Node*> def_node_;
};

} // namespace

void FreeLocals::start() {
    auto nest = Nest(old_world());
    for (auto child : nest.root()->children().nodes()) {
        auto lam = child->mut()->isa_mut<Lam>();
        if (!lam || !lam->is_set()) continue;
        auto scope = Scope(old_world(), nest, lam);
        if (!scope.run()) continue;
        for (auto& [mem, allocs] : scope.after)
            after_[mem] = std::move(allocs);
        for (auto& [pred, succs] : scope.edges)
            edges_[pred] = std::move(succs);
        for (auto [lam, tail] : scope.tails)
            tails_[lam] = tail;
        whole_.insert(scope.whole.begin(), scope.whole.end());
    }
    log().d("free_locals: {} frees after ops, {} edge frees", after_.size(), edges_.size());
    if (after_.empty() && edges_.empty()) return;
    RWPhase::start();
}

const Def* FreeLocals::rewrite(const Def* old_def) {
    if (auto i = after_.find(old_def); i != after_.end()) {
        if (auto new_def = lookup(old_def)) return new_def;
        auto mem = free_all(RWPhase::rewrite(old_def), i->second);
        return map(old_def, mem);
    }
    return RWPhase::rewrite(old_def);
}

const Def* FreeLocals::rewrite_imm_App(const App* app) {
    auto& w   = new_world();
    auto pred = curr_mut<Lam>();
    if (pred)
        if (auto i = tails_.find(pred); i == tails_.end() || i->second != app) pred = nullptr;
    if (!pred && !whole_.contains(app->arg())) return RWPhase::rewrite_imm_App(app);

    auto arg = app->arg();
    const Def* new_arg;
    if (whole_.contains(arg)) {
        auto n  = arg->num_projs();
        new_arg = w.tuple(DefVec(n, [&](size_t i) { return rewrite(arg->proj(n, i)); }));
    } else {
        new_arg = rewrite_control(arg, pred);
    }
    auto new_callee = pred ? rewrite_control(app->callee(), pred) : rewrite(app->callee());
    return w.app(new_callee, new_arg);
}

const Def* FreeLocals::rewrite_control(const Def* d, Lam* pred) {
    auto& w = new_world();
    if (auto lam = d->isa_mut<Lam>()) {
        auto new_lam = rewrite(lam);
        if (auto i = edges_.find(pred); i != edges_.end())
            if (auto j = i->second.find(lam); j != i->second.end()) return trampoline(new_lam, j->second);
        return new_lam;
    }
    if (auto tuple = d->isa<Tuple>()) {
        auto n = tuple->num_ops();
        return w.tuple(rewrite(tuple->type()),
                       DefVec(n, [&](size_t i) { return rewrite_control(tuple->op(i), pred); }));
    }
    if (auto ex = d->isa<Extract>(); ex && ex->tuple()->isa<Tuple>())
        return w.extract(rewrite_control(ex->tuple(), pred), rewrite(ex->index()));
    if (auto app = d->isa<App>(); app && app->axm())
        return w.app(rewrite_control(app->callee(), pred), rewrite_control(app->arg(), pred));
    return rewrite(d);
}

const Def* FreeLocals::trampoline(const Def* cont, const DefVec& allocs) {
    auto& w  = new_world();
    auto pi  = Pi::isa_cn(cont->type());
    auto lam = w.mut_con(pi->dom())->set("free_then");
    auto k   = mem_index(lam);
    if (!k) {
        log().d("free_locals: continuation `{}` has no leading memory, leaking {} buffers", cont, allocs.size());
        return cont;
    }
    auto n    = lam->num_vars();
    auto args = DefVec(n, [&](size_t i) { return lam->var(n, i); });
    args[*k]  = free_all(args[*k], allocs);
    lam->app(true, cont, args);
    return lam;
}

const Def* FreeLocals::free_all(const Def* mem, const DefVec& allocs) {
    for (auto alloc : allocs) {
        auto buf       = rewrite(alloc->proj(2, 1));
        auto [r, s, T] = Axm::isa<buffer::Buf>(buf->type())->args<3>();
        mem            = buffer::op_free(r, s, T, mem, buf);
    }
    return mem;
}

} // namespace mim::plug::btensor::phase
