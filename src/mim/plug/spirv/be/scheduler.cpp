#include "mim/plug/spirv/be/scheduler.h"

#include "mim/cfg.h"
#include "mim/lam.h"

namespace mim::plug::spirv {

using Node = CFG::Node;
using Loop = CFG::Loop;

/// Emit blocks in an order that keeps loop bodies contiguous after headers.
/// For each node, if it is a loop header, all blocks in that loop are emitted
/// before any block outside it — recursively for nested loops.
static void emit_loop_body(const Loop* loop, const CFG& cfg, Scheduler::Schedule& res, MutSet& done) {
    // Emit entries (headers) first.
    for (auto node : loop->entries())
        if (auto [_, ins] = done.emplace(const_cast<Lam*>(node->mut())); ins)
            res.emplace_back(const_cast<Lam*>(node->mut()));

    // Emit nested loops contiguously.
    for (auto& child : loop->children())
        emit_loop_body(child.get(), cfg, res, done);

    // Emit remaining loop body nodes.
    for (auto node : loop->nodes())
        if (auto [_, ins] = done.emplace(const_cast<Lam*>(node->mut())); ins)
            res.emplace_back(const_cast<Lam*>(node->mut()));
}

static void emit_node(const Node* node, const CFG& cfg, Scheduler::Schedule& res, MutSet& done) {
    if (auto [_, ins] = done.emplace(const_cast<Lam*>(node->mut())); !ins) return;
    res.emplace_back(const_cast<Lam*>(node->mut()));

    // If this node is a loop header, emit the entire loop body contiguously.
    if (auto loop = node->loop()) {
        // Check if this node is an entry of its loop.
        if (loop->entries().contains(node)) emit_loop_body(loop, cfg, res, done);
    }

    // Continue with successors.
    for (auto succ : node->succs())
        emit_node(succ, cfg, res, done);
}

Scheduler::Schedule schedule(const Nest& nest) {
    auto cfg = mim::nest_cfg(nest);
    Scheduler::Schedule res;
    MutSet done;
    emit_node(cfg.entry(), cfg, res, done);
    return res;
}

} // namespace mim::plug::spirv
