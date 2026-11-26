#pragma once

#include <algorithm>
#include <optional>
#include <print>
#include <sstream>
#include <utility>

#include <fe/algo.h>

#include <mim/driver.h>

#include "mim/plug/mach/asm.h"

namespace mim::plug::mach {

class Emitter;

/// Emitter::emit, compiled once into `libmim_mach`; Emitter reaches it via `GET_FUN_PTR`.
extern "C" MIM_EXPORT void mim_mach_emit(Emitter&, bool global);

/// A physical register of a register class.
struct Reg {
    size_t num;
    Target::Class cls                 = Target::Gpr;
    bool operator==(const Reg&) const = default;
};

/// The source of a copy: a register or a literal; `reg.cls` is the class of either.
struct Src {
    Reg reg        = {};
    const Def* imm = nullptr;
};

using Moves = fe::Vector<std::pair<Reg, Src>>;

/// Writes the GNU assembler code of one function on the registers that Color assigned.
/// The control flow is target-independent: jumps, calls, returns, and the entry are parallel copies, the blocks are
/// laid out by layout(), and the callee-saved registers that hold values are saved.
/// A backend derives from it and emits its instructions, moves, branches, prologue, and epilogue.
class Emitter {
public:
    Emitter(const Target& target, std::ostream& os, AsmEmit& emit, Lam* fun)
        : target_(target)
        , os_(os)
        , emit_(emit)
        , fun_(fun)
        , ret_(fun->ret_var()) {}
    virtual ~Emitter() = default;

    void emit(bool global) { fun_->world().driver().GET_FUN_PTR("mach", mim_mach_emit)(*this, global); }

protected:
    /// @name Hooks
    ///@{
    /// Emits the instruction @p app on the chain.
    virtual void step(const App* app)             = 0;
    virtual void move(Reg dst, Reg src)           = 0;
    virtual void mov_imm(Reg dst, const Def* lit) = 0;
    /// Exchanges two registers of one class; only called if there is no scratch().
    virtual void swap(Reg, Reg) { std::unreachable(); }
    /// A register of class @p cls that breaks a copy cycle instead of swap(), if any.
    virtual std::optional<Reg> scratch(Target::Class) const { return {}; }
    /// Pushes the spill slot @p i; the pops come in reverse order.
    virtual void push_slot(size_t i) = 0;
    virtual void pop_slot(size_t i)  = 0;
    /// Stores @p src, a value of @p type, into the spill slot @p i.
    virtual void spill(Src src, const Def* type, size_t i) = 0;
    /// Loads the spill slot @p i, a value of @p type, into @p dst.
    virtual void reload(Reg dst, const Def* type, size_t i) = 0;
    /// Puts the address of the @p n slots from @p first on into the register @p dst.
    virtual void slot_addr(size_t dst, size_t first, size_t n) = 0;
    virtual void jmp(std::string_view label)                   = 0;
    /// Branches to @p label if the condition code @p cc holds.
    virtual void branch_cc(size_t cc, std::string_view label) = 0;
    /// Branches to @p label if the `Bool` in register @p r is set.
    virtual void branch_if(size_t r, std::string_view label) = 0;
    /// Calls @p sym with @p num_args arguments per register class in their registers.
    virtual void call_insn(std::string_view sym, const std::array<size_t, Target::Num_Classes>& num_args) = 0;
    /// The code between the function's label and its body; slots() and saved() are final by then.
    virtual void prologue(std::ostream& os) = 0;
    virtual void epilogue()                 = 0;
    /// The argument of `.type`, e.g. `@function`.
    virtual std::string_view function_type() const { return "@function"; }
    ///@}

    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::print(body_, "\t");
        std::println(body_, fmt, std::forward<Args>(args)...);
    }
    void label_here(std::string_view label) { std::println(body_, "{}:", label); }

    const Target& target() const { return target_; }
    std::string_view backend() const { return target_.name(); }
    Lam* fun() const { return fun_; }
    std::string label(Lam* lam) const { return std::format("{}{}", Target::local_prefix(), lam->unique_name()); }
    std::string symbol(Lam* lam) { return emit_.symbol(lam); }
    nat_t nat(const Def* def) const {
        if (auto l = Lit::isa(def)) return *l;
        def->blame("{} backend: expected a literal, got `{}`", backend(), def).bail();
    }

    /// The register of the result @p def.
    size_t def_reg(const Def* def) const {
        if (auto r = fe::lookup(regs_, def)) return *r;
        def->blame("{} backend: `{}` has no register", backend(), def).bail();
    }
    /// The register of the operand @p def, a Target::in.
    size_t use_reg(const Def* def) const {
        if (Target::is_in(def)) return Target::in_reg(def);
        if (Lit::isa(def)) def->blame("{} backend: literal `{}` cannot be an operand here", backend(), def).bail();
        def->blame("{} backend: `{}` is not in a register", backend(), def).bail();
    }
    std::optional<size_t> isa_reg(const Def* def) const {
        if (auto r = fe::lookup(regs_, def)) return *r;
        return {};
    }
    Src source(const Def* def) const {
        auto cls = target_.cls(Target::strip(def));
        return Lit::isa(def) ? Src{.reg = {0, cls}, .imm = def} : Src{.reg = {use_reg(def), cls}};
    }

    /// @name Flags
    ///@{
    /// Records that the flags hold the result @p fl.
    void set_flags(const Def* fl) { flags_ = fl; }
    void clobber_flags() { flags_ = nullptr; }
    bool flags_clobbered() const { return flags_ == nullptr; }
    /// Rejects a use of the flags @p fl if something clobbered them since.
    void use_flags(const Def* fl) const {
        if (fl != flags_) fl->blame("{} backend: flags `{}` are clobbered before their use", backend(), fl).bail();
    }
    ///@}

    /// @name Frame
    /// The frame holds 8-byte slots: spill slots and the areas of `slot` instructions.
    ///@{
    /// The slot of the spilled value @p def.
    size_t spill_slot(const Def* def) {
        auto [i, ins] = slots_.emplace(def, num_slots_);
        if (ins) ++num_slots_;
        return i->second;
    }
    /// Reserves @p bytes and yields the index of their first slot.
    size_t alloc_slots(nat_t bytes) {
        auto res = num_slots_;
        num_slots_ += (bytes + 7) / 8;
        return res;
    }
    size_t num_slots() const { return num_slots_; }
    /// The callee-saved registers of class @p cls that hold values.
    const fe::Vector<size_t>& saved(Target::Class cls) const { return saved_[cls]; }
    size_t num_saved() const {
        return std::ranges::fold_left(saved_, size_t(0), [](size_t n, const auto& rs) { return n + rs.size(); });
    }
    ///@}

    std::ostringstream& body() { return body_; }

    /// The bytes of the callee-saved registers, slots, and spill slots, rounded up to keep `sp` 16-byte aligned.
    size_t frame_size() const { return fe::pad(8 * (num_saved() + num_slots_), 16); }

private:
    friend void mim_mach_emit(Emitter&, bool);

    void run(bool global);
    void insn(const App*);
    void collect(const Def*, DefSet&);
    void block(Lam* lam, const fe::Vector<const App*>& insns);
    void parallel(Moves moves, fe::Vector<std::pair<size_t, size_t>> slot_moves);
    void copy(Lam* target, const Def* arg);
    void jump(Lam* target);
    void call(Lam* callee, const Def* arg);

    const Target& target_;
    std::ostream& os_;
    AsmEmit& emit_;
    Lam* fun_;
    const Def* ret_;
    std::ostringstream body_;
    DefMap<size_t> regs_;
    DefMap<size_t> slots_;
    size_t num_slots_ = 0;
    std::array<fe::Vector<size_t>, Target::Num_Classes> saved_;
    Lam* next_        = nullptr;
    const Def* flags_ = nullptr; ///< The result of the instruction that set the flags.
};

} // namespace mim::plug::mach
