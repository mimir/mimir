#include "automaton/nfa.h"

#include "automaton/automaton.h"

namespace automaton {

void NFANode::add_transition(const NFANode* to, std::uint16_t c) {
    if (auto it = transitions_.find(c); it != transitions_.end())
        it->second.push_back(to);
    else
        transitions_.try_emplace(c, std::vector<const NFANode*>{to});
}

std::vector<const NFANode*> NFANode::get_transitions(std::uint16_t c) const {
    if (erroring_) return {};

    if (auto it = transitions_.find(c); it != transitions_.end())
        return it->second;
    else
        return {};
}

template class AutomatonBase<NFANode>;

std::ostream& operator<<(std::ostream& os, const NFANode& node) {
    auto print_char = [](std::uint16_t c) -> std::string {
        if (c == NFA::SpecialTransitons::EPSILON)
            return "ε";
        else if (c >= 48 && c <= 122)
            return {static_cast<char>(c)};
        return std::to_string(c);
    };

    return print_node(os, node, std::move(print_char));
}

} // namespace automaton
