#pragma once

#include <fe/assert.h>

#include "mim/plugin.h"

namespace mim {

class Axm : public Def, public Setters<Axm> {
private:
    Axm(NormalizeFn, u8 curry, u8 trip, const Def* type, plugin_t, tag_t, sub_t);

public:
    using Setters<Axm>::set;

    /// @name Normalization
    /// @anchor normalization
    /// For a curried App of an Axm, you only want to trigger normalization at specific spots.
    /// For this reason, MimIR maintains a Def::curry_ counter that each App decrements.
    /// The Axm::normalizer() will be triggered when Axm::curry() becomes `0`.
    /// These are also the spots that you can mim::test / mim::force / mim::IsA.
    /// After that, the counter will be set to Axm::trip().
    /// E.g., let's say an Axm has this type:
    /// ```
    /// A -> B -> C -> D -> E
    ///           ^         |
    ///           |         |
    ///           +---------+
    /// ```
    /// Using an initial value as `5` for Axm::curry and `3` as Axm::trip has the effect that here
    /// ```
    /// x a b c1 d1 e1 c2 d2 e2 c3 d3 e3
    /// ```
    /// the Axm::normalizer will be triggered after App'ing `e1`, `e2`, and `e3`.
    ///@{
    NormalizeFn normalizer() const { return normalizer_; }
    u8 curry() const { return curry_; }
    u8 trip() const { return trip_; }
    ///@}

    /// @name Normalization - Helpers
    ///@{

    /// Yields currying counter of @p def.
    /// @returns `{nullptr, 0, 0}` if no Axm is present.
    static std::tuple<const Axm*, u8, u8> get(const Def* def);
    static std::pair<u8, u8> infer_curry_and_trip(const Def* type);
    ///@}

    /// @name Annex Name
    /// @anchor anatomy
    /// @see annex_name "Annex Name"
    ///@{
    plugin_t plugin() const { return Annex::flags2plugin(flags()); }
    tag_t tag() const { return Annex::flags2tag(flags()); }
    sub_t sub() const { return Annex::flags2sub(flags()); }
    flags_t base() const { return Annex::flags2base(flags()); }
    ///@}

    /// @name IsA
    ///@{
    /// Type of IsA::def_.
    template<class T>
    struct IsANode {
        using type = App;
    };

    template<class Id, class D>
    class IsA {
        static_assert(Annex::num<Id>() != size_t(-1), "invalid number of sub tags");
        static_assert(Annex::base<Id>() != flags_t(-1), "invalid axm base");

    public:
        IsA() = default;
        IsA(const Axm* axm, const D* def)
            : axm_(axm)
            , def_(def) {}

        /// @name Getters
        ///@{
        const Axm* axm() const { return axm_; }
        const D* operator->() const { return def_; }
        operator const D*() const { return def_; }
        explicit operator bool() { return axm_ != nullptr; }
        ///@}

        /// @name Axm Name
        /// @see annex_name "Annex Name"
        ///@{
        auto plugin() const { return axm()->plugin(); } ///< @see Axm::plugin.
        auto tag() const { return axm()->tag(); }       ///< @see Axm::tag.
        auto sub() const { return axm()->sub(); }       ///< @see Axm::sub.
        auto base() const { return axm()->sub(); }      ///< @see exiom::base.
        auto id() const { return Id(axm()->flags()); }  ///< Axm::flags cast to @p Id.
        ///@}

    private:
        const Axm* axm_ = nullptr;
        const D* def_   = nullptr;
    };
    ///@}

    /// @name isa/as
    ///@{
    /// @see @ref cast_axm
    template<class Id, u8 Curry = 0, bool DynCast = true>
    static auto isa(const Def* def) {
        using D              = std::conditional_t<Curry == 0, typename Axm::IsANode<Id>::type, Def>;
        auto [axm, curry, _] = Axm::get(def);
        bool cond            = axm && curry == Curry && axm->base() == Annex::base<Id>();

        if constexpr (DynCast) return cond ? IsA<Id, D>(axm, def->as<D>()) : IsA<Id, D>();
        assert(cond && "assumed to be correct axm");
        return IsA<Id, D>(axm, def->as<D>());
    }

    template<class Id, u8 Curry = 0, bool DynCast = true>
    static auto isa(Id id, const Def* def) {
        // using D              = typename Axm::IsANode<Id>::type;
        using D              = std::conditional_t<Curry == 0, typename Axm::IsANode<Id>::type, Def>;
        auto [axm, curry, _] = Axm::get(def);
        bool cond            = axm && curry == Curry && axm->flags() == (flags_t)id;

        if constexpr (DynCast) return cond ? IsA<Id, D>(axm, def->as<D>()) : IsA<Id, D>();
        assert(cond && "assumed to be correct axm");
        return IsA<Id, D>(axm, def->as<D>());
    }

    // clang-format off
    template<class Id, u8 Curry=0> static auto as(       const Def* def) { return isa<Id, Curry, false>(    def); }
    template<class Id, u8 Curry=0> static auto as(Id id, const Def* def) { return isa<Id, Curry, false>(id, def); }
    // clang-format on
    ///@}

    static constexpr u8 Trip_End    = u8(-1);
    static constexpr auto Node      = mim::Node::Axm;
    static constexpr size_t Num_Ops = 0;

private:
    const Def* rebuild_(World&, const Def*, Defs) const final;

    friend class World;
};

// clang-format off
template<class Id> concept annex_with_subs    = Annex::num<Id>() != 0;
template<class Id> concept annex_without_subs = Annex::num<Id>() == 0;
// clang-format on

/// @name is_commutative/is_associative
///@{
template<class Id>
constexpr bool is_commutative(Id) {
    return false;
}
/// @warning By default we assume that any commutative operation is also associative.
/// Please provide a proper specialization if this is not the case.
template<class Id>
constexpr bool is_associative(Id id) {
    return is_commutative(id);
}
///@}

} // namespace mim
