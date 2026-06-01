#include "mim/schedule.h"

#include <queue>

#include "mim/world.h"

namespace mim {

Scheduler::Scheduler(const Nest& nest)
    : nest_(&nest) {
    std::queue<const Def*> queue;
    DefSet done;

    auto enqueue = [&](const Def* def, size_t i, const Def* op) {
        if (nest.contains(op)) {
            assert_emplace(def2uses_[op], def, i);
            if (auto [_, ins] = done.emplace(op); ins) queue.push(op);
        }
    };

    for (auto mut : nest.muts()) {
        queue.push(mut);
        assert_emplace(done, mut);
    }

    while (!queue.empty()) {
        auto def = queue.front();
        queue.pop();

        if (!def->is_set()) continue;

        for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
            // all reachable muts have already been registered above
            // NOTE we might still see references to unreachable muts in the schedule
            if (!def->op(i)->isa_mut()) enqueue(def, i, def->op(i));
        }

        if (!def->type()->isa_mut()) enqueue(def, -1, def->type());
    }
}

const Nest::Node* Scheduler::early(const Def* def) {
    if (auto i = early_.find(def); i != early_.end()) return i->second;
    if (def->is_closed() || !nest().contains(def)) return early_[def] = nest().root();
    if (auto var = def->isa<Var>()) return early_[def] = nest()[var->mut()];

    auto result = nest().root();
    for (auto op : def->deps()) {
        if (!op->isa_mut() && nest().contains(op)) {
            auto node = early(op);
            if (node->level() > result->level()) result = node;
        }
    }

    return early_[def] = result;
}

const Nest::Node* Scheduler::late(Def* curr_mut, const Def* def) {
    if (auto i = late_.find(def); i != late_.end()) return i->second;
    if (def->is_closed() || !nest().contains(def)) return late_[def] = nest().root();

    const Nest::Node* result = nullptr;
    if (auto mut = def->isa_mut()) {
        result = nest()[mut];
    } else if (auto var = def->isa<Var>()) {
        result = nest()[var->mut()];
    } else {
        for (auto use : uses(def)) {
            auto mut = late(curr_mut, use);
            result   = result ? Nest::lca(result, mut) : mut;
        }
    }

    if (!result) result = nest()[curr_mut];

    return late_[def] = result;
}

const Nest::Node* Scheduler::smart(Def* curr_mut, const Def* def) {
    if (auto i = smart_.find(def); i != smart_.end()) return i->second;

    auto e = early(def);
    auto l = late(curr_mut, def);
    auto s = l;

    int depth = l->loop_depth();
    for (auto i = l; i != e;) {
        i = i->inest();

        if (i == nullptr) {
            world().ELOG("this should never occur - don't know where to put {}", def);
            s = l;
            break;
        }

        if (int curr_depth = i->loop_depth(); curr_depth < depth) {
            s     = i;
            depth = curr_depth;
        }
    }

    return smart_[def] = s;
}

static void post_order(const Nest& nest, const Nest::Node* node, Scheduler::Schedule& res, MutSet& done) {
    // The mut of a virtual root is a nullptr
    if (!node->mut()) {
        for (auto child : node->children().nodes())
            post_order(nest, child, res, done);
        return;
    }

    if (!node->mut()->isa<Lam>()) return;
    if (auto [_, ins] = done.emplace(node->mut()); !ins) return;

    for (auto op : node->mut()->deps()) {
        for (auto mut : op->local_muts())
            if (auto next = nest[mut]) post_order(nest, next, res, done);
    }

    res.emplace_back(node->mut());
}

// until we have sth better ...
Scheduler::Schedule Scheduler::schedule(const Nest& nest) {
    Schedule schedule;
    MutSet done;
    post_order(nest, nest.root(), schedule, done);
    std::ranges::reverse(schedule); // post-order → reverse post-order
    return schedule;
}

} // namespace mim
