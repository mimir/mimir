#pragma once

#include <mim/phase.h>

namespace mim::plug::core::phase {

/// Lowers each dynamic Extract type `(T_0, ..., T_{n-1})#i` to the core.Union of its components.
/// The union needs no tag: whoever holds a value of such a type also holds the `i` that selects its case.
/// A value `t#i` of such a type becomes a dynamic Extract from all components of `t`, each bitcast into the union.
/// A dynamic Insert of such a value into a heterogeneous tuple selects among the results of all literal Inserts.
class LowerDynExtract : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Is the old_world() @p type a `(T_0, ..., T_{n-1})#i` with a dynamic `i`?
    static const Extract* isa_dyn(const Def* type);
    /// Converts the new_world() @p val between a core.Union and one of its cases, whichever way @p type demands.
    const Def* cast(const Def* type, const Def* val);

    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_imm_Insert(const Insert*) final;
    const Def* coerce_leaf(const Def* new_type, const Def* new_val) final;
};

} // namespace mim::plug::core::phase
