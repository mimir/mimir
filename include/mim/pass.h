#pragma once

#include <memory>
#include <stack>
#include <typeindex>

#include <fe/assert.h>
#include <fe/cast.h>

#include "mim/world.h"

namespace mim {

class Pass;
class PassMan;
using Passes = std::deque<std::unique_ptr<Pass>>;

/// @name Undo
/// Used by FPPass::analyze to indicate where to backtrack to.
///@{
using undo_t                    = size_t;
static constexpr undo_t No_Undo = std::numeric_limits<undo_t>::max();
///@}

/// Common base for Phase and Pass.
class Stage : public fe::RuntimeCast<Stage> {
public:
    /// @name Construction & Destruction
    ///@{
    Stage(World& world, std::string name)
        : world_(world)
        , name_(std::move(name)) {}
    Stage(World& world, flags_t annex);

    virtual ~Stage() = default;
    virtual std::unique_ptr<Stage> recreate(); ///< Creates a new instance; needed by a fixed-point PhaseMan.
    virtual void apply(const App*) {}          ///< Invoked if your Stage has additional args.
    virtual void apply(Stage&) {}              ///< Dito, but invoked by Stage::recreate.

    /// @name Redirection
    /// A Stage may resolve to a *different* Stage (or to nothing) after Stage::apply.
    /// This is used by the `%%compile.named_*` stages that resolve a string to another plugin's annex.
    ///@{
    virtual bool redirects() const { return false; } ///< If `true`, Stage::create uses take_resolved().
    virtual std::unique_ptr<Stage> take_resolved() {
        return {};
    } ///< The Stage to use instead; `nullptr` means *elide*.
    ///@}

    static std::unique_ptr<Stage> create(const Flags2Stages& stages, const Def* def) {
        auto& world = def->world();
        auto p_def  = App::uncurry_callee(def);
        world.DLOG("apply stage: `{}`", p_def);

        if (auto axm = p_def->isa<Axm>())
            if (auto i = stages.find(axm->flags()); i != stages.end()) {
                auto stage = i->second(world);
                if (stage) {
                    stage->apply(def->isa<App>());
                    if (stage->redirects()) return stage->take_resolved();
                }
                return stage;
            } else
                error("stage `{}` not found", axm->sym());
        else
            error("unsupported callee for a stage: `{}`", p_def);
    }

    template<class A, class P>
    static void hook(Flags2Stages& stages) {
        assert_emplace(stages, Annex::base<A>(), [](World& w) { return std::make_unique<P>(w, Annex::base<A>()); });
    }
    ///@}

    /// @name Getters
    ///@{
    World& world() { return world_; }
    Driver& driver() { return world().driver(); }
    Log& log() const { return world_.log(); }
    std::string_view name() const { return name_; }
    flags_t annex() const { return annex_; }
    ///@}

private:
    World& world_;
    flags_t annex_ = 0;

protected:
    std::string name_;
};

/// All Pass%es that want to be registered in the PassMan must implement this interface.
/// * Inherit from RWPass if your pass does **not** need state and a fixed-point iteration.
/// * Inherit from FPPass if you **do** need state and a fixed-point.
/// * If you do not rely on interaction between different Pass%es, consider using Phase instead.
class Pass : public Stage {
public:
    /// @name Construction & Destruction
    ///@{
    Pass(World& world, std::string name)
        : Stage(world, name) {}
    Pass(World& world, flags_t annex)
        : Stage(world, annex) {}

    virtual void init(PassMan*);
    ///@}

    /// @name Getters
    ///@{
    PassMan& man() { return *man_; }
    const PassMan& man() const { return *man_; }
    size_t index() const { return index_; }
    ///@}

    /// @name Rewrite Hook for the PassMan
    /// Rewrites an *immutable* @p def within PassMan::curr_mut.
    /// @returns the replacement.
    ///@{
    virtual const Def* rewrite(const Def* def) { return def; }
    virtual const Def* rewrite(const Var* var) { return var; }
    virtual const Def* rewrite(const Proxy* proxy) { return proxy; }
    ///@}

    /// @name Analyze Hook for the PassMan
    /// Invoked after the PassMan has finished Pass::rewrite%ing PassMan::curr_mut to analyze the Def.
    /// Will only be invoked if Pass::fixed_point() yields `true` - which will be the case for FPPass%es.
    /// @returns mim::No_Undo or the state to roll back to.
    ///@{
    virtual undo_t analyze(const Def*) { return No_Undo; }
    virtual undo_t analyze(const Var*) { return No_Undo; }
    virtual undo_t analyze(const Proxy*) { return No_Undo; }
    ///@}

    /// @name Further Hooks for the PassMan
    ///@{
    virtual bool fixed_point() const { return false; }

    /// Should the PassMan even consider this pass?
    virtual bool inspect() const = 0;

    /// Invoked just before Pass::rewrite%ing PassMan::curr_mut's body.
    /// @note This is invoked when seeing the *inside* of a mutable the first time.
    /// This is often too late, as you usually want to do something when you see a mutable the first time from the
    /// *outside*. This means that this PassMan::curr_mut has already been encountered elsewhere. Otherwise, we wouldn't
    /// have seen PassMan::curr_mut to begin with (unless it is Def::is_external).
    virtual void enter() {}

    /// Invoked **once** before entering the main rewrite loop.
    virtual void prepare() {}
    ///@}

    /// @name proxy
    ///@{
    const Proxy* proxy(const Def* type, Defs ops, u32 tag = 0) { return world().proxy(type, ops, index(), tag); }
    /// Check whether given @p def is a Proxy whose Proxy::pass matches this Pass's @p IPass::index.
    const Proxy* isa_proxy(const Def* def, u32 tag = 0) {
        if (auto proxy = def->isa<Proxy>(); proxy != nullptr && proxy->pass() == index() && proxy->tag() == tag)
            return proxy;
        return nullptr;
    }
    const Proxy* as_proxy(const Def* def, u32 tag = 0) {
        auto proxy = def->as<Proxy>();
        assert_unused(proxy->pass() == index() && proxy->tag() == tag);
        return proxy;
    }
    ///@}

private:
    /// @name Memory Management
    ///@{
    virtual void* alloc() { return nullptr; }           ///< Default constructor.
    virtual void* copy(const void*) { return nullptr; } ///< Copy constructor.
    virtual void dealloc(void*) {}                      ///< Destructor.
    ///@}

    PassMan* man_ = nullptr;
    size_t index_;

    friend class PassMan;
};

/// An optimizer that combines several optimizations in an optimal way.
/// This is loosely based upon:
/// "Composing dataflow analyses and transformations" by Lerner, Grove, Chambers.
class PassMan : public Pass {
public:
    PassMan(World& world, flags_t annex)
        : Pass(world, annex) {}

    void apply(Passes&&);
    void apply(const App* app) final;
    void apply(Stage& stage) final { apply(std::move(static_cast<PassMan&>(stage).passes_)); }
    void init(PassMan*) final { fe::unreachable(); }

    bool inspect() const final { fe::unreachable(); }

    /// @name Getters
    ///@{
    bool empty() const { return passes_.empty(); }
    const auto& passes() const { return passes_; }
    bool fixed_point() const { return fixed_point_; }
    Def* curr_mut() const { return curr_mut_; }
    ///@}

    /// @name Create and run Passes
    ///@{
    void run(); ///< Run all registered passes on the whole World.

    Pass* find(std::type_index key) {
        if (auto i = registry_.find(key); i != registry_.end()) return i->second;
        return nullptr;
    }

    template<class P>
    P* find() {
        if (auto pass = find(std::type_index(typeid(P)))) return static_cast<P*>(pass);
        return nullptr;
    }

    void add(std::unique_ptr<Pass>&& pass) {
        fixed_point_ |= pass->fixed_point();
        auto p        = pass.get();
        auto type_idx = std::type_index(typeid(*p));
        if (auto pass = find(type_idx)) error("already added `{}`", pass->name());
        registry_.emplace(type_idx, p);
        passes_.emplace_back(std::move(pass));
    }
    ///@}

private:
    /// @name State
    ///@{
    struct State {
        State()                 = default;
        State(const State&)     = delete;
        State(State&&)          = delete;
        State& operator=(State) = delete;
        State(size_t num)
            : data(num) {}

        Def* curr_mut = nullptr;
        DefVec old_ops;
        std::stack<Def*> stack;
        MutMap<undo_t> mut2visit;
        Vector<void*> data;
        Def2Def old2new;
        DefSet analyzed;
    };

    void push_state();
    void pop_states(undo_t undo);
    State& curr_state() {
        assert(!states_.empty());
        return states_.back();
    }
    const State& curr_state() const {
        assert(!states_.empty());
        return states_.back();
    }
    undo_t curr_undo() const { return states_.size() - 1; }
    ///@}

    /// @name rewriting
    ///@{
    const Def* rewrite(const Def*);

    const Def* map(const Def* old_def, const Def* new_def) {
        curr_state().old2new[old_def] = new_def;
        curr_state().old2new.emplace(new_def, new_def);
        return new_def;
    }

    std::optional<const Def*> lookup(const Def* old_def) {
        for (auto& state : states_ | std::views::reverse)
            if (auto i = state.old2new.find(old_def); i != state.old2new.end()) return i->second;
        return {};
    }
    ///@}

    /// @name analyze
    ///@{
    undo_t analyze(const Def*);
    bool analyzed(const Def* def) {
        for (auto& state : states_ | std::views::reverse)
            if (state.analyzed.contains(def)) return true;
        curr_state().analyzed.emplace(def);
        return false;
    }
    ///@}

    Passes passes_;
    absl::flat_hash_map<std::type_index, Pass*> registry_;
    std::deque<State> states_;
    Def* curr_mut_    = nullptr;
    bool fixed_point_ = false;
    bool proxy_       = false;

    template<class P, class M>
    friend class FPPass;
};

/// Inherit from this class using [CRTP](https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern),
/// if your Pass does **not** need state and a fixed-point iteration.
/// If you are only interested in specific mutables, you can pass this to @p M.
template<class P, class M = Def>
class RWPass : public Pass {
public:
    RWPass(World& world, std::string name)
        : Pass(world, std::move(name)) {}
    RWPass(World& world, flags_t annex)
        : Pass(world, annex) {}

    bool inspect() const override {
        if constexpr (std::is_same<M, Def>::value)
            return man().curr_mut();
        else
            return man().curr_mut()->template isa<M>();
    }

    M* curr_mut() const {
        if constexpr (std::is_same<M, Def>::value)
            return man().curr_mut();
        else
            return man().curr_mut()->template as<M>();
    }
};

/// Inherit from this class using [CRTP](https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern),
/// if you **do** need a Pass with a state and a fixed-point.
template<class P, class M = Def>
class FPPass : public RWPass<P, M> {
public:
    using Super = RWPass<P, M>;
    using Data  = std::tuple<>; ///< Default.

    FPPass(World& world, std::string name)
        : Super(world, std::move(name)) {}
    FPPass(World& world, flags_t annex)
        : Super(world, annex) {}

    bool fixed_point() const override { return true; }

protected:
    /// @name State-related Getters
    ///@{
    const auto& states() const { return Super::man().states_; }
    auto& states() { return Super::man().states_; }
    auto& data() {
        assert(!states().empty());
        return *static_cast<typename P::Data*>(states().back().data[Super::index()]);
    }
    template<size_t I>
    auto& data() {
        return std::get<I>(data());
    }
    /// Use this for your convenience if `P::Data` is a map.
    template<class K>
    auto& data(const K& key) {
        return data()[key];
    }
    /// Use this for your convenience if `P::Data<I>` is a map.
    template<size_t I, class K>
    auto& data(const K& key) {
        return data<I>()[key];
    }
    ///@}

    /// @name undo
    ///@{
    undo_t curr_undo() const { return Super::man().curr_undo(); } ///< Current undo point.

    /// Retrieves the point to backtrack to just **before** @p mut was seen the very first time.
    undo_t undo_visit(Def* mut) const {
        const auto& mut2visit = Super::man().curr_state().mut2visit;
        if (auto i = mut2visit.find(mut); i != mut2visit.end()) return i->second;
        return No_Undo;
    }

    /// Retrieves the point to backtrack to just **before** rewriting @p mut%'s body.
    undo_t undo_enter(Def* mut) const {
        for (auto i = states().size(); i-- != 0;)
            if (states()[i].curr_mut == mut) return i;
        return No_Undo;
    }
    ///@}

private:
    /// @name Memory Management for State
    ///@{
    void* alloc() override { return new typename P::Data(); }
    void* copy(const void* p) override { return new typename P::Data(*static_cast<const typename P::Data*>(p)); }
    void dealloc(void* state) override { delete static_cast<typename P::Data*>(state); }
    ///@}
};

} // namespace mim
