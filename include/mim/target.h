#pragma once

#include <string_view>

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"
#include "mim/tuple.h"

namespace mim {

/// The machine facts that the target-independent parts of a native backend need, such as Liveness and `mem.schedule`.
/// A backend plugin registers its Target via Plugin::target; see Driver::target.
/// The constructor takes the base flags of `core.M`, `mem.sched`, and the backend's axioms `slot`, `cc`, and `Flags`.
class Target {
public:
    /// The register classes; `Stack` values live in a spill slot and are unbounded.
    enum Class : size_t { Gpr, Fpr, Flags, Stack, Num_Classes };

    Target(std::string_view name, flags_t mem, flags_t sched, flags_t slot, flags_t cc, flags_t flags)
        : name_(name)
        , mem_(mem)
        , sched_(sched)
        , slot_(slot)
        , cc_(cc)
        , flags_(flags) {}
    virtual ~Target() = default;

    /// The plugin name, used as the prefix of diagnostics.
    std::string_view name() const { return name_; }
    flags_t sched() const { return sched_; }
    flags_t slot() const { return slot_; }

    /// The register class of the value @p def, from its type.
    virtual Class cls(const Def* def) const = 0;
    /// Is @p app an instruction of this backend, other than `mem.sched`, that belongs on a `mem.M 0` chain?
    virtual bool is_machine_insn(const App* app) const = 0;
    /// Is @p app an instruction that takes the `mem.M 0` it continues as its first argument?
    virtual bool is_mem_insn(const App* app) const = 0;
    /// Is @p def a register operand `in r v`?
    virtual bool is_in(const Def* def) const = 0;
    /// The value @p v of class @p cls in register @p r.
    virtual const Def* in(Class cls, size_t r, const Def* v) const = 0;
    /// The size and alignment of a pointer in bytes; see `core.trait`.
    virtual nat_t ptr_size() const = 0;

    /// Is @p def an application of the axiom with base flags @p base, with @p curry arguments to go?
    static bool is_axm(flags_t base, const Def* def, u8 curry = 0) {
        auto [axm, c, _] = Axm::get(def);
        return axm && c == curry && axm->base() == base;
    }
    /// The `f` of `mem.sched f m x`.
    static const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }
    /// The register `r` of `in r v`.
    static size_t in_reg(const Def* in) {
        return *Lit::isa(in->as<App>()->callee()->as<App>()->callee()->as<App>()->arg());
    }

    /// Is @p app an instruction that belongs on a `mem.M 0` chain?
    bool is_insn(const App* app) const { return is_axm(sched_, app) || is_machine_insn(app); }
    bool is_mem(const Def* def) const {
        auto type = def->type(); // Level has no type
        return type && is_axm(mem_, type);
    }
    /// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
    const Def* prev_mem(const App* app) const {
        if (is_axm(sched_, app)) return app->callee()->as<App>()->arg();
        return is_mem_insn(app) ? app->arg(app->num_args(), 0) : nullptr;
    }
    /// The `v` of `in r v`, otherwise @p def.
    const Def* strip(const Def* def) const { return is_in(def) ? def->as<App>()->arg() : def; }
    /// Is @p def a `Flags` value or a condition tested on one, neither of which is a register value?
    bool is_flags(const Def* def) const { return is_axm(flags_, def->type()) || is_axm(cc_, def); }
    bool is_sched_cc(const App* app) const { return is_axm(sched_, app) && is_axm(cc_, sched_insn(app), 1); }
    /// The `mem.sched` of a `cc` that yields @p cond, if any.
    const App* isa_sched_cc(const Def* cond) const {
        if (auto ex = cond->isa<Extract>())
            if (auto app = ex->tuple()->isa<App>(); app && is_sched_cc(app)) return app;
        return nullptr;
    }

private:
    std::string_view name_;
    flags_t mem_, sched_, slot_, cc_, flags_;
};

} // namespace mim
