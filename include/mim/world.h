#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <absl/container/btree_map.h>
#include <fe/arena.h>
#include <fe/log.h>
#include <fe/restore.h>
#include <fe/span.h>

#include "mim/axm.h"
#include "mim/flags.h"
#include "mim/rewrite.h"

#include "mim/util/dbg.h"

namespace mim {

template<class T>
concept Enum = std::is_enum_v<std::remove_reference_t<T>>;

class Driver;
struct Flags;

/// The World represents the whole program and manages creation of MimIR nodes (Def%s).
/// Def%s are hashed into an internal HashSet.
/// The World's factory methods just calculate a hash and lookup the Def, if it is already present, or create a new one
/// otherwise. This corresponds to value numbering.
///
/// You can create several worlds.
/// All worlds are completely independent from each other.
///
/// Note that types are also just Def%s and will be hashed as well.
class World {
public:
    /// World::get_loc together with its interned DbgKey, so pushing/popping a Loc never re-interns it.
    struct CurrLoc {
        Loc loc    = {};
        DbgKey key = {};
    };

    /// @name State
    ///@{
    struct State {
        State() = default;
        State(Sym name)
            : pod{.name = name} {}

        /// [Plain Old Data](https://en.cppreference.com/w/cpp/named_req/PODType)
        struct POD {
            u32 curr_gid     = 0;
            u32 curr_sub     = 0;
            CurrLoc curr_loc = {};
            Sym name;
            mutable bool frozen = false;
        } pod;

#ifdef MIM_ENABLE_CHECKS
        absl::flat_hash_set<uint32_t> breakpoints;
        absl::flat_hash_set<uint32_t> watchpoints;
#endif
        friend void swap(State& s1, State& s2) noexcept {
            using std::swap;
            assert((!s1.pod.curr_loc.loc || !s2.pod.curr_loc.loc) && "Why is get_loc() still set?");
            swap(s1.pod, s2.pod);
#ifdef MIM_ENABLE_CHECKS
            swap(s1.breakpoints, s2.breakpoints);
            swap(s1.watchpoints, s2.watchpoints);
#endif
        }
    };

    /// @name Construction & Destruction
    ///@{
    World& operator=(World) = delete;

    explicit World(Driver*, Sym name);
    World(Driver*, const State&);
    World(World&& other) noexcept
        : World(&other.driver(), other.state()) {
        swap(*this, other);
    }
    ~World();

    /// Inherits the State into the new World.
    /// World::curr_gid will be offset to not collide with the original World.
    std::unique_ptr<World> inherit() {
        auto s = state();
        s.pod.curr_gid += move_.sea.size();
        return std::make_unique<World>(&driver(), s);
    }
    ///@}

    /// @name Getters/Setters
    ///@{
    const State& state() const { return state_; }
    const Driver& driver() const { return *driver_; }
    Driver& driver() { return *driver_; }
    Zonker& zonker() { return zonker_; }

    Sym name() const { return state_.pod.name; }
    void set(Sym name) { state_.pod.name = name; }
    void set(std::string_view name) { state_.pod.name = sym(name); }

    /// Manage global identifier - a unique number for each Def.
    u32 curr_gid() const { return state_.pod.curr_gid; }
    u32 next_gid() { return ++state_.pod.curr_gid; }

    /// Manage run - used to track fixed-point iterations to compute Def::free_vars
    u32 curr_run() const { return data_.curr_run; }
    u32 next_run() { return ++data_.curr_run; }

    /// Retrieve compile Flags.
    Flags& flags();
    ///@}

    /// @name Loc
    ///@{
    using ScopedLoc = fe::Restore<CurrLoc>;

    Loc get_loc() const { return state_.pod.curr_loc.loc; }
    DbgKey dbg_key() const { return state_.pod.curr_loc.key; } ///< World::get_loc, already interned.
    [[nodiscard]] ScopedLoc push(Loc);

    /// Loc to blame for a diagnostic about @p def.
    /// Def%s are hash-consed, so Def::loc may belong to whatever file first created a structurally equal term.
    /// World::get_loc is the syntactic site the emitter is currently working on and thus the user's actual mistake;
    /// it is only set during emit, so fall back to the Def itself - and to its enclosing mutable - afterwards.
    Loc err_loc(const Def* def) const;
    ///@}

    /// @name Diagnostics
    ///@{
    /// Throws a single-message Error bound to this World's Driver, so it renders with its layout.
    template<class... Args>
    [[noreturn]] void error(Loc loc, std::format_string<Args...> f, Args&&... args) const {
        // Driver is incomplete here, so the Error is built in world.cpp; the lambda keeps Driver::render in charge.
        error_(loc, [&] { return std::vformat(f.get(), std::make_format_args(args...)); });
    }
    ///@}

    /// @name Sym
    ///@{
    Sym sym(std::string_view);
    Sym sym(const char*);
    Sym sym(const std::string&);
    /// Appends a @p suffix or an increasing number if the suffix already exists.
    Sym append_suffix(Sym name, std::string suffix);
    ///@}

    /// @name Freeze
    /// In frozen state the World does not create any nodes.
    ///@{
    bool is_frozen() const { return state_.pod.frozen; }

    /// Freezes the World until the end of the scope and restores the previous frozen state afterwards:
    /// ```
    /// {
    ///     auto _ = world.freeze();
    ///     // do stuff
    /// }
    /// ```
    [[nodiscard]] auto freeze() const { return fe::Restore(state_.pod.frozen, true); }
    ///@}

    /// @name Debugging Features
    ///@{
#ifdef MIM_ENABLE_CHECKS
    const auto& breakpoints() { return state_.breakpoints; }
    const auto& watchpoints() { return state_.watchpoints; }

    const Def* gid2def(u32 gid); ///< Lookup Def by @p gid.
    void breakpoint(u32 gid);    ///< Trigger breakpoint in your debugger when creating      a Def with this @p gid.
    void watchpoint(u32 gid);    ///< Trigger breakpoint in your debugger when Def::set%ting a Def with this @p gid.

    World& verify(); ///< Verifies that all externals() and annexes() are Def::is_closed(), if `MIM_ENABLE_CHECKS`.
#else
    World& verify() { return *this; }
#endif
    ///@}

    class Externals {
    public:
        ///@name Get syms/muts
        ///@{
        const auto& sym2mut() const { return sym2mut_; }
        auto syms() const { return sym2mut_ | std::views::keys; }
        auto muts() const { return sym2mut_ | std::views::values; }
        /// Returns a copy of @p muts() in a fe::Vector; this allows you to modify the Externals while iterating.
        /// @note The iteration will see all old externals, of course.
        fe::Vector<Def*> mutate() const { return {muts().begin(), muts().end()}; }
        Def* operator[](Sym name) const { return fe::lookup(sym2mut_, name); } ///< Lookup by @p name.
        size_t size() const { return sym2mut_.size(); }
        ///@}

        ///@name externalize/internalize
        ///@{
        void externalize(Def*);
        void internalize(Def*);
        ///@}

        /// @name Iterators
        ///@{
        auto begin() const { return sym2mut_.cbegin(); }
        auto end() const { return sym2mut_.cend(); }
        ///@}

        friend void swap(Externals& ex1, Externals& ex2) noexcept {
            using std::swap;
            swap(ex1.sym2mut_, ex2.sym2mut_);
        }

    private:
        absl::btree_map<Sym, Def*> sym2mut_;
    };

    class Annexes {
    public:
        struct Entry {
            Sym sym;
            const Def* def;
        };

        Annexes(Driver* driver)
            : driver_(driver) {}

        /// @name Getters
        ///@{
        Driver& driver() { return *driver_; }
        /// An annex's flags map to its full name and its Def.
        auto& flags2entry() { return flags2entry_; }
        const auto& flags2entry() const { return flags2entry_; }
        auto entries() const { return flags2entry_ | std::views::values; }
        auto defs() const {
            return entries() | std::views::transform([](const Entry& e) { return e.def; });
        }
        auto& sym2flags() { return sym2flags_; }
        const auto& sym2flags() const { return sym2flags_; }
        size_t size() const { return flags2entry_.size(); }
        ///@}

        /// @name attach
        ///@{
        const Def* attach(flags_t, Sym, const Def*);
        const Def* attach(plugin_t p, tag_t t, sub_t s, Sym sym, const Def* def) {
            return attach(Annex::flags(p, t, s), sym, def);
        }

        /// Overwrites the Def of an *already* attach()ed annex, keeping its Sym.
        /// Unlike attach(), this expects @p flags to be present; @see InplaceRWPhase.
        const Def* reattach(flags_t flags, const Def* def) {
            auto i = flags2entry_.find(flags);
            assert(i != flags2entry_.end() && "cannot reattach an annex that was never attached");
            i->second.def = def;
            def->annex_   = true;
            return def;
        }
        ///@}

        /// @name Iterators
        ///@{
        auto begin() const { return flags2entry_.cbegin(); }
        auto end() const { return flags2entry_.cend(); }
        ///@}

        friend void swap(Annexes& a1, Annexes& a2) noexcept {
            using std::swap;
            // clang-format off
            swap(a1.driver_,      a2.driver_);
            swap(a1.flags2entry_, a2.flags2entry_);
            swap(a1.sym2flags_,   a2.sym2flags_);
            // clang-format on
        }

    private:
        Driver* driver_;
        absl::btree_map<flags_t, Entry> flags2entry_; ///< Authoritative annex table; iterated in flags order.
        absl::btree_map<Sym, flags_t> sym2flags_;     ///< Reverse index: an annex's full name to its flags.
    };

    /// @name Externals & Annexes
    ///@{
    const Externals& externals() const { return move_.externals; }
    Externals& externals() { return move_.externals; }

    Annexes& annexes() { return move_.annexes; }
    const Annexes& annexes() const { return move_.annexes; }

    /// annexes() + externals().muts() in this order.
    auto roots() const {
        auto res = DefVec(); // TODO use std::views::concat - once we have C++26
        res.reserve(annexes().size() + externals().size());
        res.append_range(annexes().defs());
        res.append_range(externals().muts());
        return res;
    }

    /// Lookup annex by Sym.
    const Def* annex(Sym sym) {
        if (auto flags = lookup(annexes().sym2flags(), sym)) return annex(*flags);
        return nullptr;
    }

    /// Lookup annex by flags.
    const Def* annex(flags_t flags) {
        if (auto e = fe::lookup(annexes().flags2entry(), flags)) return e->def;
        log().e("Axm with ID `{}` not found; demangled plugin name is `{}`", flags, Annex::demangle(driver(), flags));
        return nullptr;
    }
    /// Lookup annex by Axm::id
    template<class Id>
    const Def* annex(Id id) {
        return annex(static_cast<flags_t>(id));
    }

    /// Get Axm from a plugin.
    /// Can be used to get an Axm without sub-tags.
    /// E.g. use `w.annex<mem::M>();` to get the `%mem.M` Axm.
    template<annex_without_subs id>
    const Def* annex() {
        return annex(Annex::base<id>());
    }
    ///@}

    /// @name Univ, Type, Var, Proxy, Hole
    ///@{
    const Univ* univ() { return data_.univ; }
    const Def* uinc(const Def* op, level_t offset = 1);
    template<int sort = UMax::Univ>
    const Def* umax(Defs);
    const Type* type(const Def* level);
    const Type* type_infer_univ() { return type(mut_hole_univ()); }
    template<level_t level = 0>
    const Type* type() {
        if constexpr (level == 0)
            return data_.type_0;
        else if constexpr (level == 1)
            return data_.type_1;
        else
            return type(lit_univ(level));
    }
    const Def* var(Def* mut);
    const Proxy* proxy(const Def* type, Defs ops, flags_t tag) { return unify<Proxy>(type, tag, ops); }

    Hole* mut_hole(const Def* type) { return insert<Hole>(type); }
    Hole* mut_hole_univ() { return mut_hole(univ()); }
    Hole* mut_hole_type() { return mut_hole(type_infer_univ()); }

    /// Either a value `?:?:Type ?` or a type `?:Type ?:Type ?`.
    Hole* mut_hole_infer_entity() {
        auto t   = type_infer_univ();
        auto res = mut_hole(mut_hole(t));
        assert(this == &res->world());
        return res;
    }
    ///@}

    /// @name Axm
    ///@{
    const Axm* axm(NormalizeFn n, u8 curry, u8 trip, const Def* type, plugin_t p, tag_t t, sub_t s) {
        return unify<Axm>(n, curry, trip, type, p, t, s);
    }
    const Axm* axm(const Def* type, plugin_t p, tag_t t, sub_t s) { return axm(nullptr, 0, 0, type, p, t, s); }

    /// Builds a fresh Axm with descending Axm::sub.
    /// This is useful during testing to come up with some entity of a specific type.
    /// It uses the plugin Axm::Global_Plugin and starts with `0` for Axm::sub and counts up from there.
    /// The Axm::tag is set to `0` and the Axm::normalizer to `nullptr`.
    const Axm* axm(NormalizeFn n, u8 curry, u8 trip, const Def* type) {
        return axm(n, curry, trip, type, Annex::Global_Plugin, 0, state_.pod.curr_sub++);
    }
    const Axm* axm(const Def* type) { return axm(nullptr, 0, 0, type); } ///< See above.
    ///@}

    /// @name Pi
    ///@{
    // clang-format off
    const Pi* pi(const Def* dom, const Def* codom, bool implicit = false) { return unify<Pi>(Pi::infer(dom, codom), dom, codom, implicit); }
    const Pi* pi(Defs       dom, const Def* codom, bool implicit = false) { return pi(sigma(dom), codom, implicit); }
    const Pi* pi(const Def* dom, Defs       codom, bool implicit = false) { return pi(dom, sigma(codom), implicit); }
    const Pi* pi(Defs       dom, Defs       codom, bool implicit = false) { return pi(sigma(dom), sigma(codom), implicit); }
    Pi*   mut_pi(const Def* type,                  bool implicit = false) { return insert<Pi>(type, implicit); }
    // clang-format on
    ///@}

    /// @name Cn
    /// Pi with codom mim::Bot%tom
    ///@{
    // clang-format off
    const Pi* cn(                                                       ) { return cn(sigma(   ),                   false); }
    const Pi* cn(const Def* dom,                   bool implicit = false) { return pi(      dom ,    type_bot(), implicit); }
    const Pi* cn(Defs       dom,                   bool implicit = false) { return cn(sigma(dom),                implicit); }
    const Pi* fn(const Def* dom, const Def* codom, bool implicit = false) { return cn({     dom ,    cn(codom)}, implicit); }
    const Pi* fn(Defs       dom, const Def* codom, bool implicit = false) { return fn(sigma(dom),       codom,   implicit); }
    const Pi* fn(const Def* dom, Defs       codom, bool implicit = false) { return fn(      dom , sigma(codom),  implicit); }
    const Pi* fn(Defs       dom, Defs       codom, bool implicit = false) { return fn(sigma(dom), sigma(codom),  implicit); }
    // clang-format on
    ///@}

    /// @name Lam
    ///@{
    const Def* filter(Lam::Filter filter) {
        if (auto b = std::get_if<bool>(&filter)) return lit_bool(*b);
        return std::get<const Def*>(filter);
    }
    const Lam* lam(const Pi* pi, Lam::Filter f, const Def* body) { return unify<Lam>(pi, filter(f), body); }
    Lam* mut_lam(const Pi* pi) { return insert<Lam>(pi); }
    // clang-format off
    const Lam* con(const Def* dom,                   Lam::Filter f, const Def* body) { return unify<Lam>(cn(dom        ), filter(f), body); }
    const Lam* con(Defs       dom,                   Lam::Filter f, const Def* body) { return unify<Lam>(cn(dom        ), filter(f), body); }
    const Lam* lam(const Def* dom, const Def* codom, Lam::Filter f, const Def* body) { return unify<Lam>(pi(dom,  codom), filter(f), body); }
    const Lam* lam(Defs       dom, const Def* codom, Lam::Filter f, const Def* body) { return unify<Lam>(pi(dom,  codom), filter(f), body); }
    const Lam* lam(const Def* dom, Defs       codom, Lam::Filter f, const Def* body) { return unify<Lam>(pi(dom,  codom), filter(f), body); }
    const Lam* lam(Defs       dom, Defs       codom, Lam::Filter f, const Def* body) { return unify<Lam>(pi(dom,  codom), filter(f), body); }
    const Lam* fun(const Def* dom, const Def* codom, Lam::Filter f, const Def* body) { return unify<Lam>(fn(dom,  codom), filter(f), body); }
    const Lam* fun(Defs       dom, const Def* codom, Lam::Filter f, const Def* body) { return unify<Lam>(fn(dom,  codom), filter(f), body); }
    const Lam* fun(const Def* dom, Defs       codom, Lam::Filter f, const Def* body) { return unify<Lam>(fn(dom,  codom), filter(f), body); }
    const Lam* fun(Defs       dom, Defs       codom, Lam::Filter f, const Def* body) { return unify<Lam>(fn(dom,  codom), filter(f), body); }
    Lam*   mut_con(const Def* dom                  ) { return insert<Lam>(cn(dom       )); }
    Lam*   mut_con(Defs       dom                  ) { return insert<Lam>(cn(dom       )); }
    Lam*   mut_lam(const Def* dom, const Def* codom) { return insert<Lam>(pi(dom, codom)); }
    Lam*   mut_lam(Defs       dom, const Def* codom) { return insert<Lam>(pi(dom, codom)); }
    Lam*   mut_lam(const Def* dom, Defs       codom) { return insert<Lam>(pi(dom, codom)); }
    Lam*   mut_lam(Defs       dom, Defs       codom) { return insert<Lam>(pi(dom, codom)); }
    Lam*   mut_fun(const Def* dom, const Def* codom) { return insert<Lam>(fn(dom, codom)); }
    Lam*   mut_fun(Defs       dom, const Def* codom) { return insert<Lam>(fn(dom, codom)); }
    Lam*   mut_fun(const Def* dom, Defs       codom) { return insert<Lam>(fn(dom, codom)); }
    Lam*   mut_fun(Defs       dom, Defs       codom) { return insert<Lam>(fn(dom, codom)); }
    // clang-format on
    ///@}

    /// @name Rewrite Rules
    ///@{
    const Reform* reform(const Def* dom) { return unify<Reform>(Reform::infer(dom), dom); }
    Rule* mut_rule(const Reform* type) { return insert<Rule>(type); }
    const Rule* rule(const Reform* type, const Def* lhs, const Def* rhs, const Def* guard) {
        return unify<Rule>(type, lhs, rhs, guard);
    }
    ///@}

    /// @name App
    ///@{
    template<bool Normalize = true>
    const Def* app(const Def* callee, const Def* arg);
    template<bool Normalize = true>
    const Def* app(const Def* callee, Defs args) {
        return app<Normalize>(callee, tuple(args));
    }
    const Def* raw_app(const Axm* axm, u8 curry, u8 trip, const Def* type, const Def* callee, const Def* arg);
    const Def* raw_app(const Def* type, const Def* callee, const Def* arg);
    const Def* raw_app(const Def* type, const Def* callee, Defs args) { return raw_app(type, callee, tuple(args)); }
    ///@}

    /// @name Sigma
    ///@{
    Sigma* mut_sigma(const Def* type, size_t size) { return insert<Sigma>(type, size); }
    /// A *mutable* Sigma of type @p level.
    template<level_t level = 0>
    Sigma* mut_sigma(size_t size) {
        return mut_sigma(type<level>(), size);
    }
    const Def* sigma(Defs ops);
    const Sigma* sigma() { return data_.sigma; } ///< The unit type within Type 0.
    ///@}

    /// @name Arr & Pack
    ///@{
    // clang-format off
    template<level_t level = 0>
    Arr* mut_arr() {
        return mut_arr(type<level>());
    }
    Arr * mut_arr (const Def* type) { return mut_seq(false, type)->as<Arr >(); }
    Pack* mut_pack(const Def* type) { return mut_seq(true , type)->as<Pack>(); }
    const Def* arr (const Def* arity, const Def* body) { return seq(false, arity, body); }
    const Def* pack(const Def* arity, const Def* body) { return seq(true , arity, body); }
    const Def* arr (Defs       shape, const Def* body) { return seq(false, shape, body); }
    const Def* pack(Defs       shape, const Def* body) { return seq(true , shape, body); }
    const Def* arr (u64            n, const Def* body) { return seq(false,     n, body); }
    const Def* pack(u64            n, const Def* body) { return seq(true ,     n, body); }
    const Def* arr (fe::View<u64>  shape, const Def* body) { return seq(false, shape, body); }
    const Def* pack(fe::View<u64>  shape, const Def* body) { return seq(true , shape, body); }
    const Def*  arr_unsafe(           const Def* body) { return seq_unsafe(false, body); }
    const Def* pack_unsafe(           const Def* body) { return seq_unsafe(true , body); }

    const Def* prod(bool term, Defs ops) { return term ? tuple(ops) : sigma(ops); }
    const Def* prod(bool term) { return term ? (const Def*)tuple() : (const Def*)sigma(); }
    // clang-format on
    ///@}

    /// @name Seq
    /// These either build a Pack or an Arr depending on the first argument.
    /// Oftentimes, the logic for Pack%s and Arr%ays can be quite similar; these methods help factoring such code.
    ///@{
    const Def* unit(bool is_pack) { return is_pack ? (const Def*)tuple() : sigma(); }
    Seq* mut_seq(bool is_pack, const Def* type) { return is_pack ? (Seq*)insert<Pack>(type) : insert<Arr>(type); }
    const Def* seq(bool is_pack, const Def* arity, const Def* body);
    const Def* seq(bool is_pack, Defs shape, const Def* body);
    const Def* seq(bool is_pack, u64 n, const Def* body) { return seq(is_pack, lit_nat(n), body); }
    const Def* seq(bool is_pack, fe::View<u64> shape, const Def* body) {
        return seq(is_pack, DefVec(shape, [this](u64 n) { return lit_nat(n); }), body);
    }
    const Def* seq_unsafe(bool is_pack, const Def* body) { return seq(is_pack, top_nat(), body); }
    ///@}

    /// @name Tuple
    ///@{
    const Def* tuple(Defs ops);
    /// Ascribes @p type to this tuple - needed for dependently typed and mutable Sigma%s.
    const Def* tuple(const Def* type, Defs ops);
    const Tuple* tuple() { return data_.tuple; } ///< the unit value of type `[]`
    const Def* tuple(Sym sym);                   ///< Converts @p sym to a tuple of type '«n; I8»'.
    ///@}

    /// @name Extract
    /// @see core::extract_unsafe
    ///@{
    const Def* extract(const Def* d, const Def* i);
    const Def* extract(const Def* d, u64 a, u64 i) { return extract(d, lit_idx(a, i)); }
    const Def* extract(const Def* d, u64 i) { return extract(d, Lit::as(d->arity()), i); }

    /// Builds `(f, t)#cond`.
    /// @note Expects @p cond as first, @p t as second, and @p f as third argument.
    const Def* select(const Def* cond, const Def* t, const Def* f) { return extract(tuple({f, t}), cond); }
    ///@}

    /// @name Insert
    /// @see core::insert_unsafe
    ///@{
    const Def* insert(const Def* d, const Def* i, const Def* val);
    const Def* insert(const Def* d, u64 a, u64 i, const Def* val) { return insert(d, lit_idx(a, i), val); }
    const Def* insert(const Def* d, u64 i, const Def* val) { return insert(d, Lit::as(d->arity()), i, val); }
    ///@}

    /// @name Lit
    ///@{
    const Lit* lit(const Def* type, u64 val);
    const Lit* lit_univ(u64 level) { return lit(univ(), level); }
    const Lit* lit_univ_0() { return data_.lit_univ_0; }
    const Lit* lit_univ_1() { return data_.lit_univ_1; }
    /// Def::arity of a Sigma is `lit_nat(num_ops())` and Def::num_projs reads it straight back out, so a plain
    /// World::lit would hash-cons a Lit just to launder an integer. Worth ~4% of an `-Og` Debug compile.
    static constexpr nat_t Num_Lit_Nats = 64;

    const Lit* lit_nat(nat_t a) {
        if (a >= Num_Lit_Nats) return lit(type_nat(), a);
        if (auto cached = data_.lit_nats[a]) return cached;
        return data_.lit_nats[a] = lit(type_nat(), a); // stays null while frozen - then we simply retry
    }
    const Lit* lit_nat_0() { return data_.lit_nat_0; }
    const Lit* lit_nat_1() { return data_.lit_nat_1; }
    const Lit* lit_nat_max() { return data_.lit_nat_max; }
    const Lit* lit_idx_1_0() { return data_.lit_idx_1_0; }
    // clang-format off
    const Lit* lit_i1()  { return lit_nat(Idx::bitwidth2size( 1)); }
    const Lit* lit_i8()  { return lit_nat(Idx::bitwidth2size( 8)); }
    const Lit* lit_i16() { return lit_nat(Idx::bitwidth2size(16)); }
    const Lit* lit_i32() { return lit_nat(Idx::bitwidth2size(32)); }
    const Lit* lit_i64() { return lit_nat(Idx::bitwidth2size(64)); }
    /// Constructs a Lit of type Idx of size @p size.
    /// @note `size = 0` means `2^64`.
    const Lit* lit_idx(nat_t size, u64 val) { return lit(type_idx(size), val); }
    const Lit* lit_idx_unsafe(u64 val) { return lit(type_idx(top(type_nat())), val); }

    template<class I> const Lit* lit_idx(I val) {
        static_assert(std::is_integral<I>());
        return lit_idx(Idx::bitwidth2size(sizeof(I) * 8), val);
    }

    /// Constructs a Lit @p of type Idx of size 2^width.
    /// `val = 64` will be automatically converted to size `0` - the encoding for 2^64.
    const Lit* lit_int(nat_t width, u64 val) { return lit_idx(Idx::bitwidth2size(width), val); }
    const Lit* lit_i1 (bool val) { return lit_int( 1, u64(val)); }
    const Lit* lit_i2 (u8   val) { return lit_int( 2, u64(val)); }
    const Lit* lit_i4 (u8   val) { return lit_int( 4, u64(val)); }
    const Lit* lit_i8 (u8   val) { return lit_int( 8, u64(val)); }
    const Lit* lit_i16(u16  val) { return lit_int(16, u64(val)); }
    const Lit* lit_i32(u32  val) { return lit_int(32, u64(val)); }
    const Lit* lit_i64(u64  val) { return lit_int(64, u64(val)); }
    // clang-format on

    /// Constructs a Lit of type Idx of size @p mod.
    /// The value @p val will be adjusted modulo @p mod.
    /// @note `mod == 0` is the special case for 2^64 and no modulo will be performed on @p val.
    const Lit* lit_idx_mod(nat_t mod, u64 val) { return lit_idx(mod, mod == 0 ? val : (val % mod)); }

    const Lit* lit_bool(bool val) { return data_.lit_bool[size_t(val)]; }
    const Lit* lit_ff() { return data_.lit_bool[0]; }
    const Lit* lit_tt() { return data_.lit_bool[1]; }
    ///@}

    /// @name Lattice
    ///@{
    template<bool Up>
    const Def* ext(const Def* type);
    const Def* bot(const Def* type) { return ext<false>(type); }
    const Def* top(const Def* type) { return ext<true>(type); }
    const Def* type_bot() { return data_.type_bot; }
    const Def* type_top() { return data_.type_top; }
    const Def* top_nat() { return data_.top_nat; }
    template<bool Up>
    const Def* bound(Defs ops);
    const Def* join(Defs ops) { return bound<true>(ops); }
    const Def* meet(Defs ops) { return bound<false>(ops); }
    const Def* merge(const Def* type, Defs ops);
    const Def* merge(Defs ops); ///< Infers the type using a Meet.
    const Def* inj(const Def* type, const Def* value);
    const Def* split(const Def* type, const Def* value);
    const Def* match(Defs);
    const Def* uniq(const Def* inhabitant);
    ///@}

    /// @name Globals
    /// @deprecated Will be removed.
    ///@{
    Global* global(const Def* type, bool is_mutable = true) { return insert<Global>(type, is_mutable); }
    ///@}

    /// @name Types
    ///@{
    const Nat* type_nat() { return data_.type_nat; }
    const Idx* type_idx() { return data_.type_idx; }
    /// @note `size = 0` means `2^64`.
    const Def* type_idx(const Def* size) { return app(type_idx(), size); }
    /// @note `size = 0` means `2^64`.
    const Def* type_idx(nat_t size) { return type_idx(lit_nat(size)); }

    /// Constructs a type Idx of size 2^width.
    /// `width = 64` will be automatically converted to size `0` - the encoding for 2^64.
    const Def* type_int(nat_t width) { return type_idx(lit_nat(Idx::bitwidth2size(width))); }
    // clang-format off
    const Def* type_bool() { return data_.type_bool; }
    const Def* type_i1()   { return data_.type_bool; }
    const Def* type_i2()   { return type_int( 2);    }
    const Def* type_i4()   { return type_int( 4);    }
    const Def* type_i8()   { return type_int( 8);    }
    const Def* type_i16()  { return type_int(16);    }
    const Def* type_i32()  { return type_int(32);    }
    const Def* type_i64()  { return type_int(64);    }
    // clang-format on
    ///@}

    /// @name implicit_app - Cope with implicit Arguments
    /// Places Hole%s as demanded by Pi::is_implicit() and then apps @p arg.
    ///@{
    template<bool Normalize = true>
    const Def* implicit_app(const Def* callee, const Def* arg);
    template<bool Normalize = true>
    const Def* implicit_app(const Def* callee, Defs args) {
        return implicit_app<Normalize>(callee, tuple(args));
    }
    template<bool Normalize = true>
    const Def* implicit_app(const Def* callee, nat_t arg) {
        return implicit_app<Normalize>(callee, lit_nat(arg));
    }
    template<bool Normalize = true, class E>
    const Def* implicit_app(const Def* callee, E arg)
        requires std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, nat_t> {
        return implicit_app<Normalize>(callee, lit_nat(std::to_underlying(arg)));
    }
    ///@}

    /// @name call
    /// Complete curried call of @p callee obeying implicits.
    ///@{
    template<bool Normalize = true, class T, class... Args>
    const Def* call(const Def* callee, T&& arg, Args&&... args) {
        return call<Normalize>(implicit_app<Normalize>(callee, std::forward<T>(arg)), std::forward<Args>(args)...);
    }

    /// Base case.
    template<bool Normalize = true, class T>
    const Def* call(const Def* callee, T&& arg) {
        return implicit_app<Normalize>(callee, std::forward<T>(arg));
    }

    /// Annex overload with enum instance as first argument.
    template<Enum Id, bool Normalize = true, class... Args>
    const Def* call(Id id, Args&&... args) {
        return call<Normalize>(annex(id), std::forward<Args>(args)...);
    }

    /// Annex overload with enum tempalte argument @p Id for annexes w/o subtag.
    template<class Id, bool Normalize = true, class... Args>
    requires std::is_enum_v<Id> const Def* call(Args&&... args) {
        return call<Normalize>(annex<Id>(), std::forward<Args>(args)...);
    }

    /// Annex overload with `flags_t` as first argument.
    template<bool Normalize = true, class... Args>
    const Def* call(flags_t id, Args&&... args) {
        return call<Normalize>(annex(id), std::forward<Args>(args)...);
    }
    ///@}

    /// @name Vars & Muts
    /// Manges sets of Vars and Muts.
    ///@{
    [[nodiscard]] auto& vars() { return move_.vars; }
    [[nodiscard]] auto& muts() { return move_.muts; }
    [[nodiscard]] const auto& vars() const { return move_.vars; }
    [[nodiscard]] const auto& muts() const { return move_.muts; }

    /// Yields the new body of `[mut->var() -> arg]mut`.
    /// The new body may have fewer elements as `mut->num_ops()` according to Def::reduction_offset.
    /// E.g. a Pi has a Pi::reduction_offset of 1, and only Pi::dom will be reduced - *not* Pi::codom.
    Defs reduce(const Var* var, const Def* arg);
    ///@}

    /// @name for_each
    /// Visits all closed mutables in this World.
    ///@{
    void for_each(bool elide_empty, std::function<void(Def*)>, bool schedule = false);

    template<class M>
    void for_each(bool elide_empty, std::function<void(M*)> f, bool schedule = false) {
        for_each(
            elide_empty,
            [f](Def* m) {
                if (auto mut = m->template isa<M>()) f(mut);
            },
            schedule);
    }
    ///@}

    /// @name dump/log
    ///@{
    const fe::Log& log() const;   ///< Log via `log().e("...", args)` etc.; owned by the Driver.
    void dump(std::ostream& os);  ///< Dump to @p os.
    void dump();                  ///< Dump to `std::cout`.
    void debug_dump();            ///< Dump in Debug build if World::log::level is fe::Log::Level::Debug.
    void write(const char* file); ///< Write to a file named @p file.
    void write();                 ///< Same above but file name defaults to World::name.
    ///@}

    /// @name dot
    /// GraphViz output.
    ///@{

    /// Dumps DOT to @p os, configured via @p cfg (see DotConfig).
    void dot(std::ostream& os, DotConfig cfg = {}) const;
    /// Same as above but write to @p file or `std::cout` if @p file is `nullptr`.
    void dot(const char* file = nullptr, DotConfig cfg = {}) const;
    ///@}

private:
    /// Backs World::error; @p fmt renders the message.
    [[noreturn]] void error_(Loc, const std::function<std::string()>& fmt) const;

    /// @name Put into Sea of Nodes
    ///@{
    /// Common tail of World::unify \& World::insert, right after World::allocate.
    template<class T>
    void stamp(T* def) {
        if (get_loc()) def->set(dbg_key()); // pre-interned: no Driver lookup inside this window
#ifdef MIM_ENABLE_CHECKS
        if (flags().trace_gids) std::println("{}: {} - {}", def->node_name(), def->gid(), def->flags());
#endif
    }

    template<class T, class... Args>
    const T* unify(Args&&... args) {
        auto num_ops = T::Num_Ops;
        if constexpr (T::Num_Ops == std::dynamic_extent) {
            auto&& last = std::get<sizeof...(Args) - 1>(std::forward_as_tuple(std::forward<Args>(args)...));
            num_ops     = last.size();
        }

        auto state = move_.arena.defs.state();
        auto def   = allocate<T>(num_ops, std::forward<Args>(args)...);
        assert(!def->isa_mut());
        stamp(def);

#ifdef MIM_ENABLE_CHECKS
        if (flags().reeval_breakpoints && breakpoints().contains(def->gid())) fe::breakpoint();
        for (auto op : def->ops())
            assert(&op->world() == this && "op of new Def belongs to a different World");
        assert((!def->type() || &def->type()->world() == this) && "type of new Def belongs to a different World");
#endif

        if (is_frozen()) {
            auto i = move_.sea.find(def);
            deallocate<T>(state, def);
            if (i != move_.sea.end()) return static_cast<const T*>(*i);
            return nullptr;
        }

        if (auto [i, ins] = move_.sea.emplace(def); !ins) {
            deallocate<T>(state, def);
            return static_cast<const T*>(*i);
        }

#ifdef MIM_ENABLE_CHECKS
        if (!flags().reeval_breakpoints && breakpoints().contains(def->gid())) fe::breakpoint();
#endif
        return def;
    }

    template<class T>
    void deallocate(fe::Arena::State state, const T* ptr) {
        --state_.pod.curr_gid;
        ptr->~T();
        move_.arena.defs.deallocate(state);
    }

    template<class T, class... Args>
    T* insert(Args&&... args) {
        if (is_frozen()) return nullptr;

        auto num_ops = T::Num_Ops;
        if constexpr (T::Num_Ops == std::dynamic_extent)
            num_ops = std::get<sizeof...(Args) - 1>(std::forward_as_tuple(std::forward<Args>(args)...));

        auto def = allocate<T>(num_ops, std::forward<Args>(args)...);
        stamp(def);

#ifdef MIM_ENABLE_CHECKS
        if (breakpoints().contains(def->gid())) fe::breakpoint();
#endif
        fe::assert_emplace(move_.sea, def);
        return def;
    }

#if (!defined(_MSC_VER) && defined(NDEBUG))
    struct Lock {
        Lock() { assert((guard_ = !guard_) && "you are not allowed to recursively invoke allocate"); }
        ~Lock() { guard_ = !guard_; }
        static bool guard_;
    };
#else
    struct Lock {
        ~Lock() {}
    };
#endif

    template<class T, class... Args>
    T* allocate(size_t num_ops, Args&&... args) {
        static_assert(sizeof(Def) == sizeof(T),
                      "you are not allowed to introduce any additional data in subclasses of Def");
        auto lock      = Lock();
        auto num_bytes = sizeof(Def) + sizeof(uintptr_t) * num_ops;
        auto ptr       = move_.arena.defs.allocate(num_bytes, alignof(T));
        auto res       = new (ptr) T(std::forward<Args>(args)...);
        assert(res->num_ops() == num_ops);
        return res;
    }
    ///@}

    Driver* driver_;
    Zonker zonker_;
    State state_;

    struct SeaHash {
        size_t operator()(const Def* def) const { return def->hash(); }
    };

    struct SeaEq {
        bool operator()(const Def* d1, const Def* d2) const { return d1->equal(d2); }
    };

    class Reduct {
    public:
        constexpr Reduct(size_t size) noexcept
            : size_(size) {}

        template<size_t N = std::dynamic_extent>
        constexpr auto defs() const noexcept {
            return fe::View<const Def*, N>{defs_, size_};
        }

    private:
        size_t size_;
        const Def* defs_[];

        friend class World;
    };

    /// Caches `[var -> arg]` as `f(0), .., f(n-1)`; fills *before* caching, as @p f may recursively reduce.
    template<class F>
    const Reduct* cache_reduct(const Var* var, const Def* arg, size_t n, F f) {
        auto buf    = move_.arena.substs.allocate(sizeof(Reduct) + n * sizeof(const Def*), alignof(const Def*));
        auto reduct = new (buf) Reduct(n);
        for (size_t i = 0; i != n; ++i)
            reduct->defs_[i] = f(i);
        fe::assert_emplace(move_.substs, std::pair{var, arg}, reduct);
        return reduct;
    }

    /// As above but for @p defs that have already been computed.
    const Reduct* cache_reduct(const Var* var, const Def* arg, Defs defs) {
        return cache_reduct(var, arg, defs.size(), [defs](size_t i) { return defs[i]; });
    }

    struct Move {
        Move(Driver* driver)
            : annexes(driver) {}

        struct {
            fe::Arena defs, substs;
        } arena;

        Externals externals;
        Annexes annexes;
        absl::flat_hash_set<const Def*, SeaHash, SeaEq> sea;
        fe::XTrie<Def, DefKey> muts;
        fe::XTrie<const Var, DefKey> vars;
        absl::flat_hash_map<std::pair<const Var*, const Def*>, const Reduct*> substs;

        friend void swap(Move& m1, Move& m2) noexcept {
            using std::swap;
            // clang-format off
            swap(m1.arena.defs,   m2.arena.defs);
            swap(m1.arena.substs, m2.arena.substs);
            swap(m1.sea,          m2.sea);
            swap(m1.substs,       m2.substs);
            swap(m1.vars,         m2.vars);
            swap(m1.muts,         m2.muts);
            swap(m1.externals,    m2.externals);
            swap(m1.annexes,      m2.annexes);
            // clang-format on
        }
    } move_;

    struct {
        const Univ* univ;
        const Type* type_0;
        const Type* type_1;
        const Bot* type_bot;
        const Top* type_top;
        const Def* type_bool;
        const Top* top_nat;
        const Sigma* sigma;
        const Tuple* tuple;
        const Nat* type_nat;
        const Idx* type_idx;
        const Lit* lit_univ_0;
        const Lit* lit_univ_1;
        const Lit* lit_nat_0;
        const Lit* lit_nat_1;
        const Lit* lit_nat_max;
        const Lit* lit_idx_1_0;
        std::array<const Lit*, 2> lit_bool;
        std::array<const Lit*, Num_Lit_Nats> lit_nats = {}; ///< @see World::lit_nat
        u32 curr_run                                  = 0;
    } data_;

    friend void swap(World& w1, World& w2) noexcept {
        using std::swap;
        // clang-format off
        swap(w1.driver_,  w2.driver_ );
        swap(w1.zonker_,  w2.zonker_ );
        swap(w1.state_,   w2.state_);
        swap(w1.data_,    w2.data_ );
        swap(w1.move_,    w2.move_ );
        // clang-format on

        swap(w1.data_.univ->world_, w2.data_.univ->world_);
        assert(&w1.univ()->world() == &w1);
        assert(&w2.univ()->world() == &w2);
    }
};

} // namespace mim
