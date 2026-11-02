#pragma once

#include <mim/phase.h>

namespace mim::plug::core::phase {

/// Lowers each Variant `| T_0 | ... | T_{n-1}` to a representation without sum types, chosen per Variant by its Layout.
/// Injecting encodes the value into that representation; a Match computes the tag and decodes the case for its arm.
/// Arms that are continuations become a branch, whereas all other arms are evaluated and the tag selects a result.
/// Run mim::Join2Variant first; a recursive Variant needs a boxed payload and is left alone.
class LowerVariant : public RWPhase {
public:
    using RWPhase::RWPhase;

    /// How a lowered Variant packs its tag and case; cases with a sole inhabitant carry no data.
    struct Layout {
        enum class Kind {
            /// `Idx (Σ s_i)`: every case is an `Idx s_i` - a dataless one counts as `Idx 1` - that occupies the values
            /// from `offsets[i]` on.
            /// Only chosen if it takes no more space than the largest case.
            Finite,
            /// `I64`: the tag sits in the low `bits` that the alignment of each pointer case leaves free.
            /// Every other case is dataless or an `Idx` shifted above the tag.
            Word,
            /// `[Idx n, payload]`: the payload is the only type of data or a core.Union of all of them.
            Tagged,
        };

        Kind kind;
        const Def* type = nullptr;
        DefVec cases;              ///< The lowered case types.
        fe::Vector<nat_t> offsets; ///< Kind::Finite only.
        nat_t total = 0;           ///< Kind::Finite only.
        nat_t bits  = 0;           ///< Kind::Word only.
    };

private:
    Layout layout(const Variant*);
    const Def* encode(const Layout&, size_t i, const Def* value);
    const Def* tag(const Layout&, const Def* x);
    /// What the arms of a Match on @p x receive.
    const Def* operand(const Layout&, const Def* x);
    const Def* decode(const Layout&, size_t i, const Def* operand);

    bool retype(const Axm*) final;
    const Def* rewrite_imm_Variant(const Variant*) final;
    const Def* rewrite_imm_Inj(const Inj*) final;
    const Def* rewrite_imm_Match(const Match*) final;

    DefMap<Layout> layouts_;
};

} // namespace mim::plug::core::phase
