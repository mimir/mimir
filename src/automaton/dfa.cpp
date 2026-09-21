#include "automaton/dfa.h"

#include <string>

#include "automaton/automaton.h"

namespace automaton {

void DFANode::add_transition(const DFANode* to, std::uint16_t c) { transitions_[c] = to; }

const DFANode* DFANode::get_transition(std::uint16_t c) const {
    if (erroring_) return nullptr;
    if (auto i = transitions_.find(c); i != transitions_.end()) return i->second;
    return nullptr;
}

template class AutomatonBase<DFANode>;

void DFANode::print(std::ostream& os) const {
    auto print_char = [](std::uint16_t c) -> std::string {
        if (c >= 48 && c <= 122) return {static_cast<char>(c)};
        return std::to_string(c);
    };
    print_node(os, *this, print_char);
}

} // namespace automaton
