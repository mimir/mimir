#pragma once

#include <cassert>
#include <cstdint>

#include <map>
#include <set>

#include "automaton/automaton.h"

namespace automaton {

class DFANode {
public:
    struct Lt {
        constexpr bool operator()(const DFANode* n, const DFANode* m) const noexcept { return n->id() < m->id(); }
    };

    DFANode(int id)
        : id_(id) {}

    constexpr int id() const noexcept { return id_; }
    void add_transition(const DFANode* to, std::uint16_t c);
    const DFANode* get_transition(std::uint16_t c) const;

    // F: void(const DFANode*)
    template<class F>
    void for_transitions(F&& f, std::uint16_t c) const {
        if (erroring_) return;
        if (auto it = transitions_.find(c); it != transitions_.end()) f(it->second);
    }

    // F: void(std::uint16_t, const DFANode*)
    template<class F>
    void for_transitions(F&& f) const {
        if (erroring_) return;
        for (auto& [c, to] : transitions_)
            f(c, to);
    }

    bool is_accepting() const noexcept { return accepting_; }
    void set_accepting(bool accepting) noexcept {
        assert(!(accepting && erroring_) && "state cannot be accepting and erroring");
        accepting_ = accepting;
    }

    bool is_erroring() const noexcept { return erroring_; }
    void set_erroring(bool erroring) noexcept {
        assert(!(accepting_ && erroring) && "state cannot be accepting and erroring");
        erroring_ = erroring;
    }

    friend std::ostream& operator<<(std::ostream& os, const DFANode& node);

private:
    int id_;
    // ordered map keeps for_transitions() iteration in char order - and hence deterministic
    std::map<std::uint16_t, const DFANode*> transitions_;
    bool accepting_ = false;
    bool erroring_  = false;
};

extern template class AutomatonBase<DFANode>;

class DFA : public AutomatonBase<DFANode> {
public:
    DFA()                      = default;
    DFA(const DFA&)            = delete;
    DFA& operator=(const DFA&) = delete;

    enum SpecialTransitons : std::uint16_t {};
};

template<class To>
using DFAMap = std::map<const DFANode*, To, DFANode::Lt>;
using DFASet = std::set<const DFANode*, DFANode::Lt>;

} // namespace automaton
