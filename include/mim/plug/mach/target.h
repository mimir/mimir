#pragma once

#include <optional>

#include <mim/driver.h>
#include <mim/target.h>

#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/mach/mach.h"

namespace mim::plug::mach {

/// What an instruction on the chain prescribes about the registers of its operands; registers are hardware encodings.
struct Constraint {
    DefVec ops;                                    ///< See Target::operands.
    fe::Vector<std::pair<size_t, size_t>> fixed;   ///< Non-literal operands that have to be in a given register.
    fe::Vector<std::pair<size_t, size_t>> results; ///< Results, see Target::result, that land in a given register.
    std::optional<size_t> tied;                    ///< The operand whose register the (only) result takes.
    fe::Vector<size_t> clobbers;                   ///< Registers written while the operands still occupy theirs.
};

/// The fixed operands with their registers, and the tied one with no register if it is @p live_after.
inline fe::Vector<std::pair<size_t, std::optional<size_t>>> copies(const Constraint& c, auto live_after) {
    auto res = fe::Vector<std::pair<size_t, std::optional<size_t>>>();
    for (auto [j, r] : c.fixed)
        res.emplace_back(j, r);
    if (c.tied && live_after(c.ops[*c.tied])) res.emplace_back(*c.tied, std::nullopt);
    return res;
}

/// The machine facts of a native backend that the phases of `mach` need.
/// A value's class is `Flags` for `mach.Flags`, `Stack` for `mach.Spill T`, `Fpr` for `math.F pe`, and `Gpr` otherwise.
/// A backend derives from it and adds its instructions, register file, ABI, and constraints.
class Target : public mim::Target {
public:
    /// The register classes; `Stack` values live in a spill slot and are unbounded.
    enum Class : size_t { Gpr, Fpr, Flags, Stack, Num_Classes };
    /// The object format of the assembler output.
    enum class Format { Elf, MachO };

    /// @p plugin is the backend's plugin id and @p cc the base flags of its condition-code family.
    Target(std::string_view name, plugin_t plugin, flags_t cc)
        : mim::Target(name)
        , plugin_(plugin)
        , cc_(cc) {}

    /// The Target of the one loaded backend; every backend's Target is a mach::Target.
    static const Target& of(Driver& driver, Loc loc = {}) { return static_cast<const Target&>(driver.target(loc)); }

    nat_t ptr_size() const override { return 8; }

    /// @name Object Format
    ///@{
    /// The object format of the host that the emitted assembly is assembled and linked on.
    static constexpr Format format() {
#ifdef __APPLE__
        return Format::MachO;
#else
        return Format::Elf;
#endif
    }
    static constexpr bool is_elf() { return format() == Format::Elf; }
    /// The prefix that the object format adds to a C symbol.
    static constexpr std::string_view global_prefix() { return is_elf() ? "" : "_"; }
    /// The prefix of an assembler-local label.
    static constexpr std::string_view local_prefix() { return is_elf() ? ".L" : "L"; }
    ///@}

    /// @name Register File and ABI
    /// Registers are hardware encodings within their Class.
    ///@{
    virtual size_t num_regs(Class cls) const = 0;
    /// Is @p r never available to register allocation?
    virtual bool reserved(Class cls, size_t r) const     = 0;
    virtual bool caller_saved(Class cls, size_t r) const = 0;
    virtual size_t num_arg_regs(Class cls) const         = 0;
    /// The register of the @p i-th argument of class @p cls.
    virtual size_t arg_reg(Class cls, size_t i) const = 0;
    virtual size_t ret_reg(Class cls) const           = 0;
    /// The number of registers of @p cls that register allocation may assign.
    size_t num_allocatable(Class cls) const {
        size_t n = 0;
        for (size_t r = 0, e = num_regs(cls); r != e; ++r)
            n += !reserved(cls, r);
        return n;
    }
    /// The number of those that a call may clobber.
    size_t num_caller_saved(Class cls) const {
        size_t n = 0;
        for (size_t r = 0, e = num_regs(cls); r != e; ++r)
            n += !reserved(cls, r) && caller_saved(cls, r);
        return n;
    }
    ///@}

    /// @name Instructions
    ///@{
    /// The register class of the value @p def, from its type.
    Class cls(const Def* def) const {
        auto type = def->type();
        if (Axm::isa<mach::Flags>(type)) return Flags;
        if (Axm::isa<Spill>(type)) return Stack;
        return math::isa_f(type) ? Fpr : Gpr;
    }
    /// Is @p app an instruction of this backend, other than `mach.sched`, that belongs on a `mem.M 0` chain?
    /// By default, an application of an axiom of the backend or of `mach` that is no `Pi` and no `mach.in`.
    virtual bool is_machine_insn(const App* app) const {
        auto axm = app->axm();
        return axm && (axm->plugin() == plugin_ || axm->plugin() == Plugin_Id) && !app->type()->isa<Pi>()
            && !Axm::isa<mach::in>(app);
    }
    /// Is @p app an instruction that belongs on a `mem.M 0` chain?
    bool is_insn(const App* app) const { return Axm::isa<sched>(app) || is_machine_insn(app); }
    /// Is @p app an instruction that takes the `mem.M 0` it continues as its first argument?
    /// `mach.slot`; a backend adds its loads and stores.
    virtual bool is_mem_insn(const App* app) const { return Axm::isa<slot>(app); }
    /// May @p insn go if nothing uses its results, e.g. a load?
    /// A `mach.sched` and `mach.slot`; a backend adds its loads.
    virtual bool is_removable(const App* insn) const { return Axm::isa<sched>(insn) || Axm::isa<slot>(insn); }
    /// The register constraint of @p insn; by default, it has none.
    virtual Constraint constraint(const App* insn) const {
        auto res = Constraint();
        res.ops  = operands(insn);
        return res;
    }
    /// Does the literal @p lit fit a hole of type `Want::Imm`?
    virtual bool fits_imm(const Def*) const { return true; }
    /// Is @p axm an operation that instruction selection must have replaced?
    /// An axiom of `core`, `mem`, or `math`, or `mach.load`/`mach.store`.
    static bool is_lowered(const Axm* axm) {
        auto p = axm->plugin();
        return p == core::Plugin_Id || p == mem::Plugin_Id || p == math::Plugin_Id
            || axm->flags() == Annex::Base<load> || axm->flags() == Annex::Base<store>;
    }
    /// The operands of @p insn: the argument of a `sched`'s instruction, otherwise the arguments including the memory.
    static DefVec operands(const App* insn) {
        auto ops = Axm::isa<sched>(insn) ? insn->arg()->projs() : insn->args();
        return DefVec(ops.begin(), ops.end());
    }
    /// The @p j-th projection of the non-memory result of @p insn.
    static const Def* result(const App* insn, size_t j) {
        auto res = insn->proj(2, 1);
        return res->proj(res->num_projs(), j);
    }
    /// @p insn continuing @p m with @p ops instead of its operands.
    static const App* rebuild(const App* insn, const Def* m, const DefVec& ops) {
        auto& w = insn->world();
        if (Axm::isa<sched>(insn)) return w.app(w.app(insn->callee()->as<App>()->callee(), m), w.tuple(ops))->as<App>();
        auto args = ops;
        args[0]   = m;
        return w.app(insn->callee(), w.tuple(args))->as<App>();
    }
    ///@}

    /// @name Spilling
    ///@{
    /// The instruction `f` of `sched f m x` that spills a value of @p type.
    static const Def* spill(const Def* type) { return type->world().app(type->world().annex<mach::spill>(), type); }
    /// The instruction that reloads a value of @p type from its spill slot.
    static const Def* reload(const Def* type) { return type->world().app(type->world().annex<mach::reload>(), type); }
    /// The type of the spill slot of a value of @p type.
    static const Def* spill_type(const Def* type) { return type->world().app(type->world().annex<Spill>(), type); }
    ///@}

    /// @name Registers
    ///@{
    /// Is @p def a register operand `in r v`?
    static bool is_in(const Def* def) { return Axm::isa<mach::in>(def); }
    /// The value @p v of class @p cls in register @p r.
    const Def* in(Class cls, size_t r, const Def* v) const {
        auto& w = v->world();
        return w.call<mach::in>(w.lit_idx(num_regs(cls), r), v);
    }
    /// The register `r` of `in r v`.
    static size_t in_reg(const Def* in) {
        return *Lit::isa(in->as<App>()->callee()->as<App>()->callee()->as<App>()->arg());
    }
    /// The `v` of `in r v`, otherwise @p def.
    static const Def* strip(const Def* def) { return is_in(def) ? def->as<App>()->arg() : def; }
    ///@}

    /// @name Chain
    ///@{
    /// Is @p def an application of the axiom with base flags @p base, with @p curry arguments to go?
    static bool is_axm(flags_t base, const Def* def, u8 curry = 0) {
        auto [axm, c, _] = Axm::get(def);
        return axm && c == curry && axm->base() == base;
    }
    /// The `f` of `sched f m x`.
    static const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }
    static bool is_mem(const Def* def) {
        auto type = def->type(); // Level has no type
        return type && Axm::isa<core::M>(type);
    }
    /// The memory in @p def, or `nullptr`; see `core::mem_def`.
    static const Def* mem_def(const Def* def) {
        if (is_mem(def)) return def;
        if (def->isa<Arr>() || def->type()->isa<Arr>()) return nullptr; // don't look into possibly gigantic arrays
        if (def->num_projs() > 1)
            for (auto proj : def->projs())
                if (auto m = mem_def(proj)) return m;
        return nullptr;
    }
    /// The memory parameter of @p lam, or `nullptr`.
    static const Def* mem_var(Lam* lam) { return mem_def(lam->var()); }
    /// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
    const Def* prev_mem(const App* app) const {
        if (Axm::isa<sched>(app)) return app->callee()->as<App>()->arg();
        return is_mem_insn(app) ? app->arg(app->num_args(), 0) : nullptr;
    }
    ///@}

    /// @name Flags
    ///@{
    /// Is @p def a `Flags` value or a condition tested on one, neither of which is a register value?
    bool is_flags(const Def* def) const { return Axm::isa<mach::Flags>(def->type()) || is_axm(cc_, def); }
    bool is_sched_cc(const App* app) const { return Axm::isa<sched>(app) && is_axm(cc_, sched_insn(app), 1); }
    /// The `sched` of a `cc` that yields @p cond, if any.
    const App* isa_sched_cc(const Def* cond) const {
        if (auto ex = cond->isa<Extract>())
            if (auto app = ex->tuple()->isa<App>(); app && is_sched_cc(app)) return app;
        return nullptr;
    }
    ///@}

private:
    plugin_t plugin_;
    flags_t cc_;
};

} // namespace mim::plug::mach
