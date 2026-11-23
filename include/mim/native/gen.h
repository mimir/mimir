#pragma once

#include <optional>
#include <string_view>

#include <mim/phase.h>

#include "mim/native/rule_table.h"

namespace mim::native {

/// Writes the closed rules among the annexes of a backend's isel plugin as the C++ matchers and builders of a
/// rules::Table in `mim::plug::<backend>::rules`, to `-X <isel>:o=<file>` or stdout.
/// The isel plugin derives from it and states the machine facts of its backend.
class Gen : public Phase {
public:
    /// The annexes of the isel plugin that Gen knows; `0` for one it does not have.
    struct Annexes {
        flags_t addr    = 0; ///< `addr p`: the pointer `p` as an address operand, Want::Addr.
        flags_t reg     = 0; ///< `reg p`: the pointer `p` in a register, Want::Reg.
        flags_t reg_imm = 0; ///< `reg_imm x`: Want::RegImm.
        flags_t reg_sh  = 0; ///< `reg_sh x`: Want::RegSh.
        flags_t scaled  = 0; ///< `scaled i`: Want::Scaled.
        flags_t imm     = 0; ///< The guard `imm x`: Want::Imm.
        flags_t lit     = 0; ///< The guard `lit x`: a literal.
        flags_t commutes;    ///< The tuple of the operations whose two operands a rule also matches swapped.
        flags_t formats;     ///< The values of a family parameter of type `«2; Nat»`.
        flags_t sizes;       ///< The values of a family parameter of type `Nat`.
    };

    /// @p backend names the plugin whose instructions the rules of the plugin @p isel select.
    Gen(World& world,
        flags_t annex,
        std::string_view backend,
        plugin_t backend_id,
        std::string_view isel,
        plugin_t isel_id,
        Annexes annexes)
        : Phase(world, annex)
        , backend_(backend)
        , backend_id_(backend_id)
        , isel_(isel)
        , isel_id_(isel_id)
        , annexes_(annexes) {}

    void start() final;

    std::string_view backend() const { return backend_; }
    plugin_t backend_id() const { return backend_id_; }
    std::string_view isel() const { return isel_; }
    plugin_t isel_id() const { return isel_id_; }
    const Annexes& annexes() const { return annexes_; }

    /// @name Machine Facts
    ///@{
    /// The cost of the instruction @p insn of the backend, which a rule's cost sums up.
    virtual u32 cost(const App* insn) const = 0;
    /// The operand whose register the result of the two-address instruction @p insn takes.
    virtual std::optional<size_t> tied(const App*) const { return {}; }
    /// What an operand of the backend's address type @p type is: Want::Base, Want::Index, or Want::Addr.
    virtual std::optional<rules::Want> classify(const Def*) const { return {}; }
    ///@}

private:
    std::string_view backend_;
    plugin_t backend_id_;
    std::string_view isel_;
    plugin_t isel_id_;
    Annexes annexes_;
};

} // namespace mim::native
