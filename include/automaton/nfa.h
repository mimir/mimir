#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>

#include <map>
#include <set>
#include <vector>

#include "automaton/automaton.h"

namespace automaton {
class NFANode {
public:
    struct Lt {
        constexpr bool operator()(const NFANode* n, const NFANode* m) const noexcept { return n->id() < m->id(); }
    };

    NFANode(int id)
        : id_(id) {}

    constexpr int id() const noexcept { return id_; }
    void add_transition(const NFANode* to, std::uint16_t c);
    std::vector<const NFANode*> get_transitions(std::uint16_t c) const;

    void for_transitions(std::invocable<const NFANode*> auto f, std::uint16_t c) const {
        if (erroring_) return;
        if (auto it = transitions_.find(c); it != transitions_.end())
            for (const auto& to : it->second)
                f(to);
    }

    void for_transitions(std::invocable<std::uint16_t, const NFANode*> auto f) const {
        if (erroring_) return;
        for (auto& [c, tos] : transitions_)
            for (const auto& to : tos)
                f(c, to);
    }

    bool is_accepting() const { return accepting_; }
    void set_accepting(bool accepting) {
        assert(!(accepting && erroring_) && "state cannot be accepting and erroring");
        accepting_ = accepting;
    }

    bool is_erroring() const noexcept { return erroring_; }
    void set_erroring(bool erroring) noexcept {
        assert(!(accepting_ && erroring) && "state cannot be accepting and erroring");
        erroring_ = erroring;
    }

    void print(std::ostream& os) const;

private:
    int id_;
    // ordered map keeps for_transitions() iteration in char order - and hence deterministic
    std::map<std::uint16_t, std::vector<const NFANode*>> transitions_;
    bool accepting_ = false;
    bool erroring_  = false;
};

extern template class AutomatonBase<NFANode>;

using NFASet = std::set<const NFANode*, NFANode::Lt>;

class NFA : public AutomatonBase<NFANode> {
public:
    NFA()                      = default;
    NFA(const NFA&)            = delete;
    NFA& operator=(const NFA&) = delete;

    enum SpecialTransitons : std::uint16_t {
        EPSILON = 0x8001,
    };
};

} // namespace automaton
