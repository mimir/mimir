#include "mim/schedule.h"

#include <fe/container.h>
#include <fe/worklist.h>

#include "mim/world.h"

namespace mim {

Scheduler::Scheduler(const Nest& nest)
    : nest_(&nest) {
    // Scheduling only makes sense within one scope: a *virtual* root has no scope to place anything in.
    assert(nest.root()->mut() && "Scheduler needs the Nest of a single mutable");
    auto queue = fe::BFSWorklist<DefSet>();

    auto enqueue = [&](const Def* def, size_t i, const Def* op) {
        if (nest.contains(op)) {
            fe::assert_emplace(def2uses_[op], def, i);
            queue.push(op);
        }
    };

    queue.push(nest.muts());

    while (!queue.empty()) {
        auto def = queue.pop();

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
    if (auto var = def->isa<Var>()) return early_[def] = nest()[var->binder()];

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
        result = nest()[var->binder()];
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
            world().log().e("no place found for {}", def);
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

// until we have sth better ...
Scheduler::Schedule Scheduler::schedule(const Nest& nest) {
    auto in_nest  = [&nest](Def* mut) { return nest[mut] != nullptr; };
    auto done     = MutSet();
    auto schedule = Schedule();

    // The mut of a virtual root is a nullptr.
    if (auto root = nest.root()->mut())
        post_order(root, done, schedule, in_nest, in_nest);
    else
        for (auto mut : nest.root()->children().muts())
            post_order(mut, done, schedule, in_nest, in_nest);

    std::ranges::reverse(schedule); // post-order → reverse post-order
    return schedule;
}

} // namespace mim
