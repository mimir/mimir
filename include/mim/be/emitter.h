#pragma once

#include "mim/def.h"
#include "mim/phase.h"
#include "mim/schedule.h"
#include "mim/world.h"

namespace mim {

template<class Value, class Type, class BB, class Child>
class Emitter : public NestPhase<Lam> {
private:
    constexpr const Child& child() const { return *static_cast<const Child*>(this); }
    constexpr Child& child() { return *static_cast<Child*>(this); }

    /// Internal wrapper for Emitter::emit that schedules @p def and invokes `child().emit_bb`.
    Value emit_(const Def* def) {
        auto place = scheduler_.smart(curr_lam_, def);
        auto& bb   = lam2bb_[place->mut()->template as<Lam>()];
        return child().emit_bb(bb, def);
    }

public:
    fe::Tab tab = fe::Tab::spaces();

protected:
    Emitter(World& world, std::string name, std::ostream& ostream, bool schedule = false)
        : NestPhase<Lam>(world, std::move(name), false, schedule)
        , ostream_(ostream) {}

    virtual bool direct_style() { return false; }

    std::ostream& ostream() const { return ostream_; }

    /// Recursively emits code.
    /// `mem`-typed @p def%s return sth that is `!child().is_valid(value)`.
    /// This variant asserts in this case.
    Value emit(const Def* def) {
        auto res = emit_unsafe(def);
        assert(child().is_valid(res));
        return res;
    }

    /// As above but returning `!child().is_valid(value)` is permitted.
    Value emit_unsafe(const Def* def) {
        if (auto i = globals_.find(def); i != globals_.end()) return i->second;
        if (auto i = locals_.find(def); i != locals_.end()) return i->second;

        auto val            = emit_(def);
        return locals_[def] = val;
    }

    void visit(const Nest& nest) override {
        if (!root()->is_set()) {
            child().emit_imported(root());
            return;
        }

        schedule_        = Scheduler::schedule(nest); // cached; Child::finalize needs the very same schedule
        const auto& muts = schedule_;

        // make sure that we don't need to rehash later on
        for (auto mut : muts)
            if (auto lam = mut->isa<Lam>()) lam2bb_.try_emplace(lam, BB());
        auto old_size = lam2bb_.size();

        if (!child().direct_style()) assert(root()->ret_var());

        auto fct = child().prepare();

        Scheduler new_scheduler(nest);
        swap(scheduler_, new_scheduler);

        for (auto mut : muts) {
            if (auto lam = mut->isa<Lam>()) {
                curr_lam_ = lam;
                if (!child().direct_style()) assert(lam == root() || Lam::isa_basicblock(lam));
                child().emit_epilogue(lam);
            }
        }

        child().finalize();
        locals_.clear();
        assert_unused(lam2bb_.size() == old_size && "really make sure we didn't trigger a rehash");
        // A BB never crosses a function boundary: Nest::contains is `vars().has_intersection(def->free_vars())`,
        // so a *closed* Lam is never a member of another Lam's Nest - and it cannot belong to two Nests either,
        // since a Lam free in the vars of two closed Lams would make the outer one open.
        // Every `BB&` handed out by emit_ died with the calls above, so clearing here is safe.
        // Without it, Child::finalize re-walks the BBs of all previously emitted functions - O(n²) in program size.
        lam2bb_.clear();
    }

    /// The Scheduler::schedule of the function currently being emitted; see Emitter::visit.
    const Scheduler::Schedule& schedule() const { return schedule_; }

    Lam* curr_lam_ = nullptr;
    std::ostream& ostream_;
    Scheduler scheduler_;
    Scheduler::Schedule schedule_;
    DefMap<Value> locals_;
    DefMap<Value> globals_;
    DefMap<Type> types_;
    LamMap<BB> lam2bb_;
};

} // namespace mim
