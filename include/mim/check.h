#pragma once

#include <absl/container/flat_hash_set.h>

#include "mim/def.h"

namespace mim {

class Prod;
class Seq;

/// This node is a hole in the IR that is inferred by its context later on.
/// It is modelled as a *mutable* Def.
/// If inference was successful, it's Hole::op will be set to the inferred Def.
/// @note Hole%s are not type-checked as they are used during type-checking - causing a chicken-and-egg problem.
class Hole : public Def, public Setters<Hole> {
private:
    Hole(const Def* type)
        : Def(Node, type, 1, 0) {}

public:
    using Setters<Hole>::set;

    /// @name op
    ///@{
    const Def* op() const { return Def::op(0); }

    /// Transitively walks up Hole%s until the last one while path-compressing everything.
    /// @returns the final Hole in the chain and final op() (if any).
    std::pair<Hole*, const Def*> find();
    ///@}

    /// @name set/unset
    ///@{
    Hole* set(const Def* op) {
        assert(op != this);
        return Def::set({op})->as<Hole>();
    }
    Hole* unset() { return Def::unset()->as<Hole>(); }
    ///@}

    Hole* stub(const Def* type) { return Def::stub(world(), type)->as<Hole>(); }

    /// If unset, explode to Tuple.
    /// @returns the new Tuple, or `this` if unsuccessful.
    const Def* tuplefy(nat_t);

    /// @name isa
    ///@{
    static const Def* isa_set(const Def* def) {
        if (auto hole = def->isa<Hole>(); hole && hole->is_set()) return hole->op();
        return nullptr;
    }

    static Hole* isa_unset(const Def* def) {
        if (auto hole = def->isa_mut<Hole>(); hole && !hole->is_set()) return hole;
        return nullptr;
    }

    /// If @p def is a Hole, find last in chain, otherwise yields @p def again.
    static const Def* find(const Def* def) {
        if (auto hole = def->isa_mut<Hole>()) {
            auto [last, op] = hole->find();
            return op ? op : last;
        }
        return def;
    }
    ///@}

    static constexpr auto Node      = mim::Node::Hole;
    static constexpr size_t Num_Ops = 1;

private:
    friend class World;
    friend class Checker;
};

class Checker {
public:
    explicit Checker(World& world)
        : world_(world) {}

    World& world() { return world_; }

    enum Mode {
        /// In Mode::Check, type inference is happening and Hole%s will be resolved, if possible.
        /// Also, two *free* but *different* Var%s **are** considered α-equivalent.
        Check,
        /// In Mode::Test, no type inference is happening and Hole%s will not be touched.
        /// Also, Two *free* but *different* Var%s are **not** considered α-equivalent.
        Test,
    };

    template<Mode mode>
    static bool alpha(const Def* d1, const Def* d2) {
        if (d1 == d2) return true;
        return Checker(d1->world()).alpha_<mode>(d1, d2);
    }

    /// Can @p value be assigned to sth of @p type?
    /// @note This is different from `equiv(type, value->type())` since @p type may be dependent.
    [[nodiscard]] static const Def* assignable(const Def* type, const Def* value) {
        if (type == value->type()) return value;
        return Checker(type->world()).assignable_(type, value);
    }

    /// Yields `defs.front()`, if all @p defs are Check::alpha-equivalent (`Mode::Test`) and `nullptr` otherwise.
    static const Def* is_uniform(Defs defs);

private:
#ifdef MIM_ENABLE_CHECKS
    template<Mode>
    bool fail();
    const Def* fail();
#else
    template<Mode>
    bool fail() {
        return false;
    }
    const Def* fail() { return {}; }
#endif

    [[nodiscard]] const Def* assignable_(const Def* type, const Def* value);
    template<Mode>
    [[nodiscard]] bool alpha_(const Def* d1, const Def* d2);
    template<Mode>
    [[nodiscard]] bool alpha_impl_(const Def* d1, const Def* d2);
    template<Mode>
    [[nodiscard]] bool check(const Prod*, const Def*);
    template<Mode>
    [[nodiscard]] bool check(const Seq*, const Def*);
    [[nodiscard]] bool check1(const Seq*, const Def*);
    [[nodiscard]] bool check(Seq*, const Seq*);
    [[nodiscard]] bool check(const UMax*, const Def*);

    /// Symmetric key for the alpha_ memo: the two gids packed into one u64, smaller first.
    /// Canonicalizing by gid is what lets alpha_ get away with a single probe.
    static constexpr u64 memo_key(const Def* d1, const Def* d2) noexcept {
        auto g1 = u64(d1->gid()), g2 = u64(d2->gid());
        return g1 < g2 ? g1 << 32 | g2 : g2 << 32 | g1;
    }

    auto bind(Def* mut, const Def* d) {
        if (!mut) return std::pair(binders_.end(), true);
        auto res = binders_.emplace(mut, d);
        // A new binding may change how bound Var%s compare, so positive memo entries may become invalid.
        if (res.second)
            for (auto& memo : memo_)
                memo.clear();
        return res;
    }

    World& world_;
    MutMap<const Def*> binders_;
    std::array<absl::flat_hash_set<u64>, 2> memo_;
};

} // namespace mim
