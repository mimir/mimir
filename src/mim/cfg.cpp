#include "mim/cfg.h"

#include <algorithm>
#include <functional>
#include <stack>
#include <vector>

#include "mim/axm.h"
#include "mim/nest.h"
#include "mim/tuple.h"

namespace mim {

/*
 * Nest helpers
 */

CFG nest_cfg(const Nest::Node* node) { return CFG{node}; }

CFG nest_cfg(const Nest& nest) {
    assert(nest.root()->mut() && "nest_cfg() requires a non-virtual root");
    return nest_cfg(nest.root());
}

/*
 * CFG::Node
 */

void CFG::Node::follow_escaping(const App* app) {
    for (auto mut : app->arg()->local_muts()) {
        if (auto lam = mut->isa<Lam>()) {
            if (auto target = cfg_.visit(lam)) {
                succs_.insert(target);
                target->preds_.insert(this);
            }
        }
    }
}

bool CFG::Node::add_edge(const Lam* succ) {
    if (auto node = cfg_.visit(succ)) {
        succs_.insert(node);
        node->preds_.insert(this);
        return true;
    }
    return false;
}

void CFG::Node::init() {
    if (!mut_->is_set()) return;
    auto app = mut_->body()->as<App>();
    if (auto callee = app->callee()->isa<Lam>()) {
        if (!add_edge(callee)) follow_escaping(app);
    } else if (auto dispatch = Dispatch(app)) {
        bool follow = false;
        for (auto branch : dispatch.tuple()->ops())
            if (!add_edge(branch->as<Lam>())) follow = true;
        if (follow) follow_escaping(app);
    } else if (handle_sflow(app)) {
        // edges added by handle_sflow
    } else {
        follow_escaping(app);
    }
}

/// The scope token of an `If`/`Switch`/`Loop` capability: the first explicit
/// argument of its type. Plugin-agnostic (compares axiom names by string).
static const Def* sflow_scope_token(const Def* cf_struct) {
    auto type        = cf_struct->type();
    auto [axm, _, __] = Axm::get(type);
    if (!axm) return nullptr;
    auto sv  = axm->sym().view();
    auto app = type->as<App>();
    if (sv == "%sflow.If") return app->uncurry_args<2>()[0];
    if (sv == "%sflow.Switch") return app->uncurry_args<3>()[0];
    if (sv == "%sflow.Loop") return app->uncurry_args<5>()[0];
    return nullptr;
}

bool CFG::Node::handle_sflow(const App* app) {
    auto [axm, _curry, _trip] = Axm::get(app);
    if (!axm) return false;

    auto sv = axm->sym().view();

    auto add_lam = [&](const Def* def) {
        if (auto lam = def->isa_mut<Lam>()) add_edge(lam);
    };

    // Register an if/switch/loop constructor's argument tuple under its scope
    // token (op(0)) so that exits can recover their targets later.
    auto register_cf = [&](const Def* sigma) { cfg_.sflow_token_to_args_[sigma->op(0)] = sigma; };
    // Recover the constructor argument tuple owning a capability value.
    auto cf_args = [&](const Def* cf_struct) -> const Def* {
        auto it = cfg_.sflow_token_to_args_.find(sflow_scope_token(cf_struct));
        return it == cfg_.sflow_token_to_args_.end() ? nullptr : it->second;
    };

    // For axm positions / uncurry arities, mirror mim::plug::spirv::Emitter::emit_bb in
    // src/mim/plug/spirv/be/bb.cpp.
    if (sv == "%sflow.if") {
        auto [sigma, _arg]                      = app->uncurry_args<2>();
        register_cf(sigma);
        auto [_token, _cf_break, tuple, _index] = sigma->projs<4>();
        for (auto branch : tuple->ops()) add_lam(branch);
        return true;
    }
    if (sv == "%sflow.switch") {
        auto [sigma, _arg]                                  = app->uncurry_args<2>();
        register_cf(sigma);
        auto [_token, _cf_break, cf_default, cases, _index] = sigma->projs<5>();
        add_lam(cf_default);
        // cases: array of dependent tuples (index, continuation)
        for (auto c : cases->ops()) add_lam(c->op(1));
        return true;
    }
    if (sv == "%sflow.header") {
        // Loop pre-header / anchor: branches unconditionally into the loop header.
        auto [sigma, _arg]      = app->uncurry_args<2>();
        auto [_token, cf_header] = sigma->projs<2>();
        add_lam(cf_header);
        return true;
    }
    if (sv == "%sflow.loop") {
        // Loop header dispatch: op(0) is the `Loop` capability, so register by its
        // scope token. Record this lam as the loop header for loopbacks, and add
        // edges to the body (cond false) and break (cond true, immediate break).
        auto [sigma, _arg]                                    = app->uncurry_args<2>();
        auto [cf_struct, cf_break, _cf_continue, cf_body, _c] = sigma->projs<5>();
        cfg_.sflow_token_to_args_[sflow_scope_token(cf_struct)] = sigma;
        cfg_.sflow_loop_header_[sflow_scope_token(cf_struct)]   = mut_;
        add_lam(cf_body);
        add_lam(cf_break);
        return true;
    }
    if (sv == "%sflow.continue") {
        auto [sigma, _val]             = app->uncurry_args<2>();
        auto [_inner_token, cf_struct] = sigma->projs<2>();
        if (auto a = cf_args(cf_struct)) add_lam(a->op(2)); // loop continue
        return true;
    }
    if (sv == "%sflow.fallthrough") {
        auto [sigma, _val]             = app->uncurry_args<2>();
        auto [_inner_token, cf_struct] = sigma->projs<2>();
        if (auto a = cf_args(cf_struct)) {
            auto cases        = a->op(3);
            const Def* target = a->op(2); // default
            for (size_t i = 0, e = cases->num_ops(); i != e; ++i)
                if (cases->op(i)->op(1) == mut_) {
                    if (i > 0) target = cases->op(i - 1)->op(1);
                    break;
                }
            add_lam(target);
        }
        return true;
    }
    if (sv == "%sflow.break.s" || sv == "%sflow.break.l") {
        auto [sigma, _val]             = app->uncurry_args<2>();
        auto [_inner_token, cf_struct] = sigma->projs<2>();
        if (auto a = cf_args(cf_struct)) add_lam(a->op(1)); // break target
        return true;
    }
    if (sv == "%sflow.merge") {
        auto [sigma, _val]             = app->uncurry_args<2>();
        auto [_merge_token, cf_struct] = sigma->projs<2>();
        if (auto a = cf_args(cf_struct)) add_lam(a->op(1)); // if merge target
        return true;
    }
    if (sv == "%sflow.loopback") {
        auto [sigma, _value]              = app->uncurry_args<2>();
        auto [_loopback_token, cf_struct] = sigma->projs<2>();
        auto it = cfg_.sflow_loop_header_.find(sflow_scope_token(cf_struct));
        if (it != cfg_.sflow_loop_header_.end()) add_lam(it->second); // loop header
        return true;
    }
    if (sv == "%sflow.branch") {
        auto [callee, _token, _val] = app->uncurry_args<3>();
        add_lam(callee);
        return true;
    }
    if (sv == "%sflow.call") {
        // Returning function call; target (ret lam) lives outside the nest,
        // reached via escaping muts in the arg.
        follow_escaping(app);
        return true;
    }
    return false;
}

const CFG::Loop* CFG::Node::loop() const {
    cfg_.loops();
    while (loop_) {
        auto* before = loop_;
        before->children();
        if (loop_ == before) break;
    }
    return loop_;
}

/*
 * CFG
 */

CFG::CFG(const Nest::Node* entry)
    : world_(entry->mut()->world())
    , nest_entry_(entry)
    , entry_(mut2node_.emplace(entry->mut()->as<Lam>(), std::unique_ptr<Node>(new Node(*this, entry->mut()->as<Lam>())))
                 .first->second.get()) {
    entry_->init();
    calc_dominance();
}

CFG::Node* CFG::visit(const Lam* mut) {
    if (!mut->is_set()) return nullptr;
    auto& nest  = nest_entry_->nest();
    auto target = nest[const_cast<Lam*>(mut)];
    if (!target || !nest_entry_->nest_contains(target)) return nullptr;
    if (auto node = (*this)[mut]) return node;
    mut2node_.emplace(mut, std::unique_ptr<Node>(new Node(*this, mut)));
    mut2node_[mut]->init();
    return mut2node_[mut].get();
}

void CFG::assign_postorder_numbers() {
    size_t number = 0;
    absl::flat_hash_set<Node*> visited;

    std::function<void(Node*)> visit = [&](Node* node) {
        if (!visited.insert(node).second) return;
        for (auto succ : node->succs_)
            visit(succ);
        node->postorder_number_ = ++number;
    };

    visit(entry_);
}

/// Calculates dominance using Cooper-Harvey-Kennedy algorithm
/// from Cooper et al, "A Simple, Fast Dominance Algorithm".
/// https://www.clear.rice.edu/comp512/Lectures/Papers/TR06-33870-Dom.pdf
void CFG::calc_dominance() {
    assign_postorder_numbers();

    // entry dominates itself
    entry_->idom_ = entry_;

    // collect nodes in reverse postorder (skip entry)
    std::vector<Node*> nodes;
    nodes.reserve(mut2node_.size());
    for (auto& [_, node] : mut2node_)
        if (node.get() != entry_ && node->postorder_number_ != 0) nodes.push_back(node.get());
    std::ranges::sort(nodes, [](Node* a, Node* b) { return a->postorder_number_ > b->postorder_number_; });

    for (bool todo = true; todo;) {
        todo = false;
        for (auto node : nodes) {
            const Node* new_idom = nullptr;
            for (auto pred : node->preds_)
                if (pred->idom_) new_idom = new_idom ? lca(new_idom, pred) : pred;
            if (node->idom_ != new_idom) {
                node->idom_ = new_idom;
                todo        = true;
            }
        }
    }
}

const CFG::Node* CFG::lca(const Node* n, const Node* m) {
    while (n != m) {
        while (n->postorder_number_ < m->postorder_number_)
            n = n->idom_;
        while (m->postorder_number_ < n->postorder_number_)
            m = m->idom_;
    }
    return n;
}

/*
 * SCCs / Loops
 */

/// Computes SCCs of @p nodes using Tarjan's algorithm. Mirrors
/// Nest::Node::tarjan in structure. Edges leaving @p nodes are ignored.
/// Edges into @p blocked are also ignored.
CFG::SCCs CFG::compute_sccs(const absl::flat_hash_set<const Node*>& nodes,
                            const absl::flat_hash_set<const Node*>& blocked) const {
    // Reset scratch state on every node we are about to visit.
    for (auto n : nodes) {
        auto* m      = const_cast<Node*>(n);
        m->idx_      = Node::Unvisited;
        m->on_stack_ = false;
    }

    std::stack<Node*> stack;
    SCCs sccs;

    std::function<uint32_t(Node*, uint32_t)> tarjan = [&](Node* curr, uint32_t i) -> uint32_t {
        curr->idx_ = curr->low_ = i++;
        curr->on_stack_         = true;
        stack.emplace(curr);

        for (auto dep : curr->succs_) {
            if (!nodes.contains(dep) || blocked.contains(dep)) continue;
            if (dep->idx_ == Node::Unvisited) i = tarjan(dep, i);
            if (dep->on_stack_) curr->low_ = std::min(curr->low_, dep->low_);
        }

        if (curr->idx_ == curr->low_) {
            sccs.emplace_back(std::make_unique<SCC>());
            SCC* scc = sccs.back().get();
            Node* node;
            do {
                node            = pop(stack);
                node->on_stack_ = false;
                node->low_      = curr->idx_;
                scc->emplace(node);
            } while (node != curr);
        }

        return i;
    };

    for (uint32_t i = 0; auto cn : nodes) {
        if (blocked.contains(cn)) continue;
        auto* n = const_cast<Node*>(cn);
        if (n->idx_ == Node::Unvisited) i = tarjan(n, i);
    }

    return sccs;
}

std::unique_ptr<CFG::Loop> CFG::make_loop(const SCC& scc, const Loop* parent) {
    auto loop    = std::unique_ptr<Loop>(new Loop(parent));
    loop->nodes_ = scc;

    // Mark each node with its (currently) deepest containing loop. When nested
    // loops are computed lazily later, they will overwrite this for nodes
    // belonging to a more deeply nested loop.
    for (auto n : scc)
        const_cast<Node*>(n)->loop_ = loop.get();

    // Entries: nodes in the SCC that have a predecessor outside the SCC
    // (or are the entry of the CFG itself).
    for (auto n : scc) {
        bool is_entry = false;
        for (auto pred : n->preds_) {
            if (!scc.contains(pred)) {
                is_entry = true;
                break;
            }
        }
        if (is_entry) loop->entries_.emplace(n);
    }

    // Exits: nodes in the SCC that have a successor outside the SCC.
    for (auto n : scc) {
        for (auto succ : n->succs_) {
            if (!scc.contains(succ)) {
                loop->exits_.emplace(n);
                break;
            }
        }
    }

    return loop;
}

static bool is_loop_scc(const CFG::SCC& scc) {
    if (scc.size() > 1) return true;
    // self-loop?
    auto n = *scc.begin();
    for (auto succ : n->succs())
        if (succ == n) return true;
    return false;
}

void CFG::find_loops() const {
    absl::flat_hash_set<const Node*> all;
    all.reserve(mut2node_.size());
    for (auto& [_, node] : mut2node_)
        all.emplace(node.get());

    auto sccs = compute_sccs(all);
    for (auto& scc : sccs) {
        if (!is_loop_scc(*scc)) continue;
        loops_.emplace_back(make_loop(*scc, nullptr));
    }
}

void CFG::Loop::find_nested_loops() const {
    // Recompute SCCs within this loop's nodes, but block edges into this
    // loop's entries — this breaks back edges and exposes nested loops.
    // Only the immediate next level is computed; deeper levels are computed
    // lazily when their children() is accessed.
    auto& cfg = (*nodes_.begin())->cfg_;
    auto sccs = cfg.compute_sccs(nodes_, entries_);
    for (auto& scc : sccs) {
        if (!is_loop_scc(*scc)) continue;
        // Skip the trivial case where the nested SCC equals this loop's SCC.
        if (scc->size() == nodes_.size()) continue;
        children_.emplace_back(make_loop(*scc, this));
    }
}

} // namespace mim
