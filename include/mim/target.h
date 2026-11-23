#pragma once

#include <optional>
#include <string_view>

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"
#include "mim/tuple.h"

namespace mim {

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

/// The machine facts that the target-independent parts of a native backend need, such as Liveness and register
/// allocation. A backend plugin registers its Target via Plugin::target; see Driver::target.
class Target {
public:
    /// The register classes; `Stack` values live in a spill slot and are unbounded.
    enum Class : size_t { Gpr, Fpr, Flags, Stack, Num_Classes };
    /// The object format of the assembler output.
    enum class Format { Elf, MachO };

    /// The base flags of the annexes that libmim knows only through the Target.
    struct Annexes {
        flags_t mem;    ///< `core.M`.
        flags_t sched;  ///< `sched`, which places a pure instruction on the chain.
        flags_t slot;   ///< `slot n m`, which allocates `n` bytes of the stack frame.
        flags_t spill;  ///< `spill T`, the instruction that spills a value of type `T`.
        flags_t reload; ///< `reload T`, the instruction that reloads it.
        flags_t cc;     ///< The backend's condition-code family.
        flags_t flags;  ///< The type `Flags`.
    };

    Target(std::string_view name, Annexes annexes)
        : name_(name)
        , annexes_(annexes) {}
    virtual ~Target() = default;

    /// The plugin name, used as the prefix of diagnostics.
    std::string_view name() const { return name_; }
    const Annexes& annexes() const { return annexes_; }
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

    /// The register class of the value @p def, from its type.
    virtual Class cls(const Def* def) const = 0;
    /// Is @p app an instruction that takes the `mem.M 0` it continues as its first argument?
    virtual bool is_mem_insn(const App* app) const = 0;
    /// Is @p def a register operand `in r v`?
    virtual bool is_in(const Def* def) const = 0;
    /// The value @p v of class @p cls in register @p r.
    virtual const Def* in(Class cls, size_t r, const Def* v) const = 0;
    /// The size and alignment of a pointer in bytes; see `core.trait`.
    virtual nat_t ptr_size() const = 0;
    /// Is @p axm an operation that instruction selection must have replaced?
    virtual bool is_lowered(const Axm* axm) const = 0;

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
    size_t num_allocatable(Class cls) const;
    /// The number of those that a call may clobber.
    size_t num_caller_saved(Class cls) const;
    ///@}

    /// @name Instructions
    ///@{
    /// The register constraint of @p insn; by default, it has none.
    virtual Constraint constraint(const App* insn) const {
        auto res = Constraint();
        res.ops  = operands(insn);
        return res;
    }
    /// May @p insn go if nothing uses its results, e.g. a load?
    virtual bool is_removable(const App* insn) const { return is_axm(annexes_.sched, insn); }
    /// The instruction `f` of `sched f m x` that spills a value of class @p cls and @p type, or `nullptr`.
    virtual const Def* spill(Class, const Def*) const { return nullptr; }
    /// The instruction that reloads a value of class @p cls and @p type from its spill slot.
    virtual const Def* reload(Class, const Def*) const { return nullptr; }
    /// The type of the spill slot of a value of class @p cls and @p type.
    virtual const Def* spill_type(Class, const Def*) const { return nullptr; }
    /// The operands of @p insn: the argument of a `sched`'s instruction, otherwise the arguments including the memory.
    DefVec operands(const App* insn) const;
    /// The @p j-th projection of the non-memory result of @p insn.
    static const Def* result(const App* insn, size_t j);
    /// @p insn continuing @p m with @p ops instead of its operands.
    const App* rebuild(const App* insn, const Def* m, const DefVec& ops) const;
    ///@}

    /// Is @p def an application of the axiom with base flags @p base, with @p curry arguments to go?
    static bool is_axm(flags_t base, const Def* def, u8 curry = 0) {
        auto [axm, c, _] = Axm::get(def);
        return axm && c == curry && axm->base() == base;
    }
    /// The `f` of `sched f m x`.
    static const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }
    /// The register `r` of `in r v`.
    static size_t in_reg(const Def* in) {
        return *Lit::isa(in->as<App>()->callee()->as<App>()->callee()->as<App>()->arg());
    }

    bool is_mem(const Def* def) const {
        auto type = def->type(); // Level has no type
        return type && is_axm(annexes_.mem, type);
    }
    /// The memory in @p def, or `nullptr`; see `core::mem_def`.
    const Def* mem_def(const Def* def) const;
    /// The memory parameter of @p lam, or `nullptr`.
    const Def* mem_var(Lam* lam) const { return mem_def(lam->var()); }
    /// The memory @p app continues, if @p app is an instruction on a `mem.M 0` chain.
    const Def* prev_mem(const App* app) const {
        if (is_axm(annexes_.sched, app)) return app->callee()->as<App>()->arg();
        return is_mem_insn(app) ? app->arg(app->num_args(), 0) : nullptr;
    }
    /// The `v` of `in r v`, otherwise @p def.
    const Def* strip(const Def* def) const { return is_in(def) ? def->as<App>()->arg() : def; }
    bool is_sched_cc(const App* app) const {
        return is_axm(annexes_.sched, app) && is_axm(annexes_.cc, sched_insn(app), 1);
    }
    /// The `sched` of a `cc` that yields @p cond, if any.
    const App* isa_sched_cc(const Def* cond) const {
        if (auto ex = cond->isa<Extract>())
            if (auto app = ex->tuple()->isa<App>(); app && is_sched_cc(app)) return app;
        return nullptr;
    }

private:
    std::string_view name_;
    Annexes annexes_;
};

} // namespace mim
