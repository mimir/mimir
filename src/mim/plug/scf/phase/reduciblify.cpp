#include "mim/plug/scf/phase/reduciblify.h"

#include <ranges>

namespace mim::plug::scf::phase {

void Reduciblifier::visit(const Nest& nest) { visit_node(nest.root()); }

bool Reduciblifier::is_reducible(const Nest::Node* header) {
    for (auto node : header->sibl_deps<false>()) {
        // Ignore forward edges.
        if (header->postorder_number() > node->postorder_number()) continue;

        // Check if header dominates node with back edge.
        auto dom = node->idom();
        while (dom->postorder_number() < header->postorder_number())
            dom = dom->idom();

        if (dom != header) return false;
    }

    return true;
}

void Reduciblifier::visit_node(const Nest::Node* node) {
    for (auto& scc : node->topo()) {
        for (auto child : *scc) {
            // Make deeper loops reducible first.
            visit_node(child);
        }

        if (scc->size() > 1) {
            // TODO: Check if loop actually has multiple entries
        }
    }
}

} // namespace mim::plug::scf::phase
