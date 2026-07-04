#pragma once

#include <memory>

#include <fe/assert.h>
#include <fe/cast.h>

#include "mim/world.h"

namespace mim {

/// Common base for Phase and Repl.
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

} // namespace mim
