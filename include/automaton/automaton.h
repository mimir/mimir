#pragma once

#include <concepts>

#include <algorithm>
#include <list>
#include <map>
#include <ostream>
#include <print>
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

    void print(std::ostream& os) const {
        if constexpr (std::is_same_v<NodeType, DFANode>)
            std::println(os, "digraph dfa {{");
        else if constexpr (std::is_same_v<NodeType, NFANode>)
            std::println(os, "digraph nfa {{");
        else
            std::println(os, "digraph automaton {{");
        std::println(os, "  start -> \"{}\";", start_->id());

        for (const auto& node : nodes_)
            node.print(os);
        std::println(os, "}}");
    }

    friend std::ostream& operator<<(std::ostream& os, const AutomatonBase& automaton) {
        automaton.print(os);
        return os;
    }

private:
    std::list<NodeType> nodes_;
    const NodeType* start_ = nullptr;
    int id_                = 0;
};

template<class NodeType>
void print_node(std::ostream& os, const NodeType& node, std::invocable<std::uint64_t> auto print_char) {
    if (node.is_accepting()) std::println(os, "  \"{}\" [shape=doublecircle];", node.id());
    if (node.is_erroring()) std::println(os, "  \"{}\" [shape=square];", node.id());

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
            auto chars = lo == hi ? print_char(lo) : std::format("{}-{}", print_char(lo), print_char(hi));
            auto nums  = lo == hi ? std::format("{}", lo) : std::format("{}-{}", lo, hi);
            std::println(os, "  \"{}\" -> \"{}\" [label=\"{} ({})\"];", node.id(), to->id(), chars, nums);
        }
    }
}

} // namespace automaton
