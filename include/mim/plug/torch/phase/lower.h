#pragma once

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>

#include <mim/phase.h>

namespace mim::plug::torch::phase {

/// Torch-local implementation decomposition. Keeping this utility private to
/// the plugin avoids imposing an experimental decomposition API on MimIR core.
class DecomposeByImpl : public RWPhase {
public:
    DecomposeByImpl(World& world, flags_t annex)
        : RWPhase(world, annex) {}

protected:
    template<class Src, class Impl>
    void bind() {
        bind(Annex::base<Src>(), Annex::base<Impl>());
    }

    template<auto Src, auto Impl>
    void bind() {
        bind(flags_t(Src), flags_t(Impl));
    }

    template<class Id>
    void observe(std::string key) {
        residual_counters_.emplace(Annex::base<Id>(), std::move(key));
    }

private:
    void bind(flags_t src, flags_t impl) {
        if (!impls_.emplace(src, impl).second)
            fe::throwf("duplicate implementation decomposition source `{}`", src);
    }

    const Def* rewrite_imm_App(const App*) final;
    void rewrite_external(Def*) final;

    const Def* apply_impl(const App*, flags_t);
    const Def* decompose_generated(const Def*);
    void verify_closed(const Def*);

    std::map<flags_t, flags_t> impls_;
    std::map<flags_t, std::string> residual_counters_;
    std::unordered_set<const Def*> active_;
    Def2Def generated_;
    DefSet observed_;
};

/// Replaces each Torch framework axiom with its Mim lambda implementation.
class Lower : public DecomposeByImpl {
public:
    Lower(World&, flags_t);

protected:
    Lower(World&, flags_t, bool selective);

private:
    template<class Src, class Impl>
    void bind_if_enabled(std::string_view name) {
        if (!preserved_.erase(std::string(name))) bind<Src, Impl>();
    }

    template<auto Src, auto Impl>
    void bind_sub_if_enabled(std::string_view name, std::string_view legacy_name) {
        auto enabled = !preserved_.erase(std::string(name));
        enabled &= !preserved_.erase(std::string(legacy_name));
        if (enabled) bind<Src, Impl>();
    }

    std::set<std::string> preserved_;
};

/// Decomposes Torch operators except those named by `-X torch:preserve=...`.
class Decompose : public Lower {
public:
    Decompose(World&, flags_t);
};

} // namespace mim::plug::torch::phase
