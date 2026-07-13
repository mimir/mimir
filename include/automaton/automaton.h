#pragma once

#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <vector>

#include "automaton/range_helper.h"

namespace automaton {

class DFANode;
class NFANode;

template<class NodeType>
class AutomatonBase {
public:
    AutomatonBase()                                = default;
    AutomatonBase(const AutomatonBase&)            = delete;
    AutomatonBase& operator=(const AutomatonBase&) = delete;

    NodeType* add_state() {
        nodes_.emplace_back(id_++);
        return &nodes_.back();
    }

    void set_start(const NodeType* start) { start_ = start; }

    const NodeType* get_start() const { return start_; }

    /// Ordered by NodeType::Lt (i.e. by id) so that iteration is deterministic.
    std::set<const NodeType*, typename NodeType::Lt> get_reachable_states() const {
        std::set<const NodeType*, typename NodeType::Lt> reachableStates;
        std::vector<const NodeType*> workList;
        workList.push_back(get_start());
        while (!workList.empty()) {
            auto state = workList.back();
            workList.pop_back();
            reachableStates.insert(state);
            state->for_transitions([&](auto, auto to) {
                if (!reachableStates.contains(to)) workList.push_back(to);
            });
        }
        return reachableStates;
    }

    friend std::ostream& operator<<(std::ostream& os, const AutomatonBase& automaton) {
        if constexpr (std::is_same_v<NodeType, DFANode>)
            os << "digraph dfa {\n";
        else if constexpr (std::is_same_v<NodeType, NFANode>)
            os << "digraph nfa {\n";
        else
            os << "digraph automaton {\n";
        os << "  start -> \"" << automaton.start_->id() << "\";\n";

        for (auto& node : automaton.nodes_)
            os << node;
        os << "}\n";
        return os;
    }

private:
    std::list<NodeType> nodes_;
    const NodeType* start_ = nullptr;
    int id_                = 0;
};

template<class NodeType, class PrintCharF>
std::ostream& print_node(std::ostream& os, const NodeType& node, PrintCharF&& print_char) {
    if (node.is_accepting()) os << "  \"" << node.id() << "\" [shape=doublecircle];\n";
    if (node.is_erroring()) os << "  \"" << node.id() << "\" [shape=square];\n";

    std::map<const NodeType*, std::vector<Range>, typename NodeType::Lt> node2transitions;
    node.for_transitions([&](auto c, auto to) {
        if (!node2transitions.contains(to))
            node2transitions.try_emplace(to, std::vector<Range>{
                                                 Range{c, c}
            });
        else
            node2transitions[to].push_back({c, c});
    });

    for (auto& [to, ranges] : node2transitions) {
        std::sort(ranges.begin(), ranges.end(), RangeCompare{});
        ranges = merge_ranges(ranges);
        for (auto& [lo, hi] : ranges) {
            os << "  \"" << node.id() << "\" -> \"" << to->id() << "\" [label=\""
               << std::forward<PrintCharF>(print_char)(lo);
            if (lo != hi) os << "-" << std::forward<PrintCharF>(print_char)(hi);
            os << " (" << lo;
            if (lo != hi) os << "-" << hi;
            os << ")\"];\n";
        }
    }

    return os;
}

} // namespace automaton
