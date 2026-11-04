#pragma once

#include <bit>

#include <mim/phase.h>

namespace mim::plug::core::phase {

/// Lowers each Variant `| T_0 | ... | T_{n-1}` to a representation without sum types, chosen per Variant by its Layout.
/// Injecting encodes the value into that representation; a Match computes the tag and decodes the case for its arm.
/// Arms that are continuations become a branch, whereas all other arms are evaluated and the tag selects a result.
/// Run mim::Join2Variant first; a recursive Variant needs a boxed payload and is left alone.
class LowerVariant : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// How a lowered Variant packs its tag and case; cases with a sole inhabitant carry no data.
    struct Layout {
        enum class Kind {
            /// `Idx (Σ s_i)`: case `i`, an `Idx s_i` or dataless as `Idx 1`, takes the values from `offsets[i]` on.
            /// Only chosen if it takes no more space than the largest case.
            Finite,
            /// `I64`: the tag sits in the low bits() that the alignment of each pointer case leaves free.
            /// Every other case is dataless or an `Idx` shifted above the tag.
            Word,
            /// `[Idx n, payload]`: the payload is the only type of data or a core.Union of all of them.
            Tagged,
        };

        Kind kind;
        const Def* type = nullptr;
        DefVec cases;              ///< The lowered case types.
        fe::Vector<nat_t> offsets; ///< Kind::Finite only.

        nat_t total() const { return *Idx::isa_lit(type); }             ///< Kind::Finite only.
        nat_t bits() const { return std::bit_width(cases.size() - 1); } ///< Kind::Word only.
        nat_t mask() const { return (nat_t(1) << bits()) - 1; }         ///< Kind::Word only.
    };

    Layout layout(const Variant*);
    /// @name Layouts
    /// Each tries its Layout::Kind on the Layout::cases of @p l and fills in the rest on success.
    ///@{
    bool finite(Layout& l);
    bool word(Layout& l);
    void tagged(Layout& l);
    ///@}

    const Def* encode(const Layout&, size_t i, const Def* value);
    const Def* tag(const Layout&, const Def* x);
    /// Decodes case @p i from what a Match arm receives: the payload of a Layout::Kind::Tagged value, else the value.
    const Def* decode(const Layout&, size_t i, const Def* operand);

    bool retype(const Axm*) final;
    const Def* rewrite_imm_Variant(const Variant*) final;
    const Def* rewrite_imm_Inj(const Inj*) final;
    const Def* rewrite_imm_Match(const Match*) final;

    DefMap<Layout> layouts_;
};

} // namespace mim::plug::core::phase
