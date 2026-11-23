#pragma once

#include <mim/target.h>

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/mach/mach.h"

namespace mim::plug::mach {

/// A Target whose machine-independent axioms are those of the plugin `mach`.
/// A value's class is `Flags` for `mach.Flags`, `Stack` for `mach.Spill T`, `Fpr` for `math.F pe`, and `Gpr` otherwise.
class Target : public mim::Target {
public:
    /// @p plugin is the backend's plugin id and @p cc the base flags of its condition-code family.
    Target(std::string_view name, plugin_t plugin, flags_t cc)
        : mim::Target(name,
                      {
                          .mem    = Annex::Base<core::M>,
                          .sched  = Annex::Base<mem::sched>,
                          .slot   = Annex::Base<slot>,
                          .spill  = Annex::Base<mach::spill>,
                          .reload = Annex::Base<mach::reload>,
                          .cc     = cc,
                          .flags  = Annex::Base<mach::Flags>,
                      })
        , plugin_(plugin) {}

    Class cls(const Def* def) const override {
        auto type = def->type();
        if (Axm::isa<mach::Flags>(type)) return Flags;
        if (Axm::isa<Spill>(type)) return Stack;
        return math::isa_f(type) ? Fpr : Gpr;
    }
    /// An application of an axiom of the backend or of `mach` that is no `Pi` and no `mach.in`.
    bool is_machine_insn(const App* app) const override {
        auto axm = app->axm();
        return axm && (axm->plugin() == plugin_ || axm->plugin() == Plugin_Id) && !app->type()->isa<Pi>()
            && !Axm::isa<mach::in>(app);
    }
    /// `mach.slot`; a backend adds its loads and stores.
    bool is_mem_insn(const App* app) const override { return Axm::isa<slot>(app); }
    bool is_in(const Def* def) const final { return Axm::isa<mach::in>(def); }
    const Def* in(Class cls, size_t r, const Def* v) const final {
        auto& w = v->world();
        return w.call<mach::in>(w.lit_idx(num_regs(cls), r), v);
    }
    nat_t ptr_size() const override { return 8; }
    /// An axiom of `core`, `mem`, or `math`.
    bool is_lowered(const Axm* axm) const final {
        auto p = axm->plugin();
        return p == core::Plugin_Id || p == mem::Plugin_Id || p == math::Plugin_Id;
    }
    const Def* strip_ptr_cast(const Def* def) const final { return mem::strip_ptr_cast(def); }

    /// `mach.slot` besides what mim::Target removes; a backend adds its loads.
    bool is_removable(const App* insn) const override {
        return mim::Target::is_removable(insn) || Axm::isa<slot>(insn);
    }
    const Def* spill(Class, const Def* type) const final {
        return type->world().app(type->world().annex<mach::spill>(), type);
    }
    const Def* reload(Class, const Def* type) const final {
        return type->world().app(type->world().annex<mach::reload>(), type);
    }
    const Def* spill_type(Class, const Def* type) const final {
        return type->world().app(type->world().annex<Spill>(), type);
    }

private:
    plugin_t plugin_;
};

} // namespace mim::plug::mach
