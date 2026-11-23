#include "mim/plug/arm/phase/color.h"

#include <optional>
#include <ranges>

#include <mim/plugin.h>

#include "mim/plug/arm/liveness.h"

namespace mim::plug::arm::phase {

namespace {

/// The allocatable registers, caller-saved first, so that callee-saved ones stay free for values across calls.
const auto Allocatable = [] {
    auto res = fe::Vector<size_t>();
    for (auto saved : {true, false})
        for (size_t r = 0; r != Num_Regs; ++r)
            if (!reserved(r) && Caller_Saved[r] == saved) res.emplace_back(r);
    return res;
}();

class Colorer {
public:
    Colorer(Lam* fun)
        : fun_(fun)
        , live_(fun) {}

    void run();

private:
    void add(const Def* v);
    void link(const Def* a, const Def* b) {
        auto i = fe::lookup(ids_, a);
        auto j = fe::lookup(ids_, b);
        if (!i || !j || *i == *j) return;
        nbrs_[*i].emplace_back(*j);
        nbrs_[*j].emplace_back(*i);
    }

    Lam* fun_;
    Liveness live_;
    DefMap<size_t> ids_;
    DefVec values_;
    fe::Vector<fe::Vector<size_t>> nbrs_;
    fe::Vector<std::optional<size_t>> colors_;
    DefMap<size_t> regs_;
};

void Colorer::add(const Def* v) {
    ids_[v] = values_.size();
    values_.emplace_back(v);
    nbrs_.emplace_back();
    colors_.emplace_back();
}

void Colorer::run() {
    auto used = DefSet();
    mim::for_each_use(live_, [&](const Def* u) { used.emplace(u); });
    auto keep = [&](const Def* v) { return is_value(v, Liveness::Gpr, target()) && used.contains(v); };

    // Block discovery order visits a dominator first.
    for (const auto& block : live_.blocks()) {
        for (auto x : live_.params(block))
            if (keep(x)) add(x);
        for (auto insn : block.insns)
            for (auto d : live_.defs(insn))
                if (keep(d)) add(d);
    }

    auto across_call = DefSet();
    for (const auto& block : live_.blocks()) {
        mim::interferences(
            live_, block, [&](const Def* v) { return ids_.contains(v); },
            [&](const Def* a, const Def* b) { link(a, b); });
        if (auto k = mim::call_cont(live_, fun_, block)) across_call.insert(k->live_in.begin(), k->live_in.end());
    }

    for (size_t i = 0; i != values_.size(); ++i) {
        auto saved_only = across_call.contains(values_[i]);
        auto free       = [&](size_t r) {
            return !(saved_only && Caller_Saved[r])
                && std::ranges::none_of(nbrs_[i], [&](size_t j) { return colors_[j] == r; });
        };
        auto r = std::ranges::find_if(Allocatable, free);
        if (r == Allocatable.end())
            fun_->blame(MIM_ARM_BE "cannot assign a register to `{}` in `{}`", values_[i], fun_).bail();
        colors_[i]        = *r;
        regs_[values_[i]] = *r;
    }

    auto assign = mim::Assign(fun_->world(), Liveness::Gpr, live_, regs_);
    mim::rewrite_blocks(live_, assign);
}

} // namespace

void Color::visit(Lam* fun) {
    if (Lam::isa_mut_returning(fun)) Colorer(fun).run();
}

} // namespace mim::plug::arm::phase
