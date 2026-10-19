#include "mim/nest.h"

#include <queue>

#include "mim/world.h"

namespace mim {

Nest::Nest(Def* r)
    : world_(r->world())
    , root_(make_node(r)) {
    populate(false);
    analyze();
}

Nest::Nest(World& world, fe::View<Def*> muts, bool transitive)
    : world_(world)
    , root_(make_node(nullptr)) {
    for (auto mut : muts)
        make_node(mut, root_);
    populate(transitive);
    analyze();
}

Nest::Nest(World& world)
    : Nest(world, world.externals().mutate(), true) {}

void Nest::populate(bool transitive) {
    std::queue<Node*> queue;

    if (root()->mut())
        queue.push(root_);
    else
        for (auto child : root_->children_.mut_nodes())
            queue.push(child);

    while (!queue.empty()) {
        auto curr_node = fe::pop(queue);
        for (auto op : curr_node->mut()->deps()) {
            for (auto local_mut : op->local_muts()) {
                if (node(local_mut)) continue;
                if (!contains(local_mut)) {
                    if (transitive && local_mut->is_closed()) queue.push(make_node(local_mut, root_));
                    continue;
                }

                queue.push(make_node(local_mut, innermost(local_mut)));
            }
        }
    }
}

Nest::Node* Nest::innermost(Node* res, Vars vars) const {
    for (auto fv : vars)
        if (auto n = node(fv->binder()); n && n->level() > res->level()) res = n;
    return res;
}

Nest::Node* Nest::innermost(const Def* def) const {
    if (auto mut = def->isa_mut()) return innermost(root_, mut->free_vars());
    auto res = innermost(root_, def->local_vars());
    for (auto mut : def->local_muts())
        res = innermost(res, mut->free_vars());
    return res;
}

Nest::Node* Nest::make_node(Def* mut, Node* inest) {
    auto node = std::unique_ptr<Node>(new Node(*this, mut, inest)); // can't use make_unique - c'tor is private
    auto res  = node.get();
    mut2node_.emplace(mut, std::move(node));
    if (mut) {
        if (auto var = mut->has_var()) vars_ = world().vars().insert(vars_, var);
    }
    return res;
}

void Nest::analyze() {
    auto path = fe::Vector<Node*>();
    calc_sibl_deps(root_, path);
    calc_SCCs(root_);

    size_t n = 0;
    if (root_->mut()) {
        number(root_, n);
    } else {
        for (auto child : root_->children_.mut_nodes())
            number(child, n);
        root_->postorder_number_ = ++n;
    }

    for (auto& [mut, node] : mut2node_)
        if (mut && node->children().num() != 0) calc_dominance(node.get());
}

/// @p path holds the Node%s from the root() down to @p curr.
void Nest::calc_sibl_deps(Node* curr, fe::Vector<Node*>& path) {
    path.emplace_back(curr);
    if (curr->mut()) {
        for (auto op : curr->mut()->deps()) {
            for (auto local_mut : op->local_muts()) {
                if (auto local_node = node(local_mut)) {
                    if (local_node->is_root()) local_node->recursive_ = true;
                    if (local_node == curr) {
                        local_node->link(local_node);
                    } else if (auto inest = local_node->inest_) {
                        // Only the child of inest that leads to curr depends on local_node.
                        if (auto l = inest->level() + 1; l < path.size() && path[l - 1] == inest) {
                            assert(inest->children().contains(path[l]->mut()));
                            path[l]->link(local_node);
                        }
                    }
                }
            }
        }
    }

    for (auto child : curr->children_.mut_nodes())
        calc_sibl_deps(child, path);
    path.pop_back();
}

void Nest::calc_SCCs(Node* curr) {
    Node::Stack stack;
    for (uint32_t i = 0; auto child : curr->children_.mut_nodes())
        if (child->idx_ == Node::Unvisited) i = child->tarjan(i, stack);

    for (auto child : curr->children_.mut_nodes()) {
        child->loop_depth_ = child->is_recursive() ? curr->loop_depth() + 1 : curr->loop_depth();
        calc_SCCs(child);
    }
}

uint32_t Nest::Node::tarjan(uint32_t i, Stack& stack) {
    this->idx_ = this->low_ = i++;
    this->on_stack_         = true;
    stack.emplace(this);

    for (auto dep : this->sibl_deps_.nodes_) {
        if (dep->idx_ == Unvisited) i = dep->tarjan(i, stack);
        if (dep->on_stack_) this->low_ = std::min(this->low_, dep->low_);
    }

    if (this->idx_ == this->low_) {
        inest_->topo_.emplace_front(std::make_unique<SCC>());
        SCC* scc = inest_->topo_.front().get();
        Node* node;
        int num = 0;
        do {
            node             = fe::pop(stack);
            node->on_stack_  = false;
            node->recursive_ = true;
            node->low_       = this->idx_;
            ++num;

            scc->emplace(node);
            node->scc_ = scc;
        } while (node != this);

        if (num == 1 && !this->sibl_deps_.contains(this)) this->recursive_ = false;
    }

    return i;
}

void Nest::number(Node* node, size_t& n) {
    if (node->postorder_number_) return; // already visited
    node->postorder_number_ = 0;         // mark in progress
    for (auto op : node->mut()->deps())
        for (auto mut : op->local_muts())
            if (auto succ = this->node(mut)) number(succ, n);
    node->postorder_number_ = ++n;
}

const Nest::Node* Nest::lca(const Node* n, const Node* m) {
    while (n != m)
        if (n->postorder_number_ < m->postorder_number_)
            n = n->idom_ ? n->idom_ : n->inest();
        else if (m->postorder_number_ < n->postorder_number_)
            m = m->idom_ ? m->idom_ : m->inest();

    return n;
}

void Nest::sibl_post_order(Node* node, ankerl::unordered_dense::set<const Node*>& visited, fe::Vector<Node*>& nodes) {
    if (!visited.insert(node).second) return;
    for (auto succ : node->sibl_deps_.nodes_)
        sibl_post_order(succ, visited, nodes);
    nodes.push_back(node);
}

/// Calculates dominance among the children of @p parent using Cooper-Harvey-Kennedy algorithm
/// from Cooper et al, "A Simple, Fast Dominance Algorithm".
/// https://www.clear.rice.edu/comp512/Lectures/Papers/TR06-33870-Dom.pdf
void Nest::calc_dominance(Node* parent) {
    // Initialize entry nodes directly referenced by the parent
    for (auto op : parent->mut()->deps()) {
        for (auto local_mut : op->local_muts())
            if (auto node = this->node(local_mut); node && node->inest_ == parent) node->idom_ = parent;
    }

    // Holds all siblings in reverse post-order coming from the parent
    ankerl::unordered_dense::set<const Node*> visited;
    fe::Vector<Node*> nodes;
    for (auto op : parent->mut()->deps()) {
        for (auto mut : op->local_muts())
            if (auto node = this->node(mut); node && node->inest_ == parent) sibl_post_order(node, visited, nodes);
    }

    // Actual dominance algorithm
    for (bool todo = true; todo;) {
        todo = false;
        for (auto node : nodes | std::views::reverse) {
            // skip entry nodes
            if (node->idom_ == parent) continue;

            const Node* new_idom = nullptr;
            for (auto user : node->sibl_rev_deps_.nodes_)
                if (user->idom_) new_idom = new_idom ? lca(new_idom, user) : user;
            if (node->idom_ != new_idom) {
                node->idom_ = new_idom;
                todo        = true;
            }
        }
    }
}

} // namespace mim
