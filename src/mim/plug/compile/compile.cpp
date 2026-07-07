#include "mim/plug/compile/compile.h"

#include <memory>

#include <mim/config.h>
#include <mim/driver.h>
#include <mim/phase.h>

#include <mim/phase/beta_red.h>
#include <mim/phase/branch_normalize.h>
#include <mim/phase/eta_conv.h>
#include <mim/phase/lam_spec.h>
#include <mim/phase/prefix_cleanup.h>
#include <mim/phase/ret_wrap.h>
#include <mim/phase/scalarize.h>
#include <mim/phase/tail_rec_elim.h>

#include "mim/plug/compile/autogen.h"

using namespace mim;
using namespace mim::plug;

/// Phase hook for `%compile.named`.
/// Reads the fully-qualified annex name (e.g. `"clos.clos_conv"`) from the driving App at phase-build time,
/// looks up the matching annex `Def` in the current `World`, and *redirects* Phase::create to that annex's own
/// Phase. If the plugin part of the name is not loaded or the annex is missing, it elides (resolves to nothing),
/// so the enclosing `%compile.phases` simply skips it.
class Named : public Phase {
public:
    Named(World& w, flags_t a)
        : Phase(w, a) {}

    void start() final { fe::unreachable(); } // a Named always redirects and never runs itself

    void apply(const App* app) final {
        if (!app) return;
        auto str = tuple2str(app->arg());
        if (str.empty()) return;

        auto dot = str.find('.');
        if (dot == std::string::npos) return;
        auto begin = str[0] == '%' ? 1uz : 0uz; // skip the leading '%' of the annex name
        if (!driver().is_loaded(driver().sym(str.substr(begin, dot - begin)))) return;

        if (auto def = world().annex(driver().sym(str))) resolved_ = Phase::create(driver().phases(), def);
    }

    bool redirects() const override { return true; }
    std::unique_ptr<Phase> take_resolved() override { return std::move(resolved_); }

private:
    std::unique_ptr<Phase> resolved_;
};

void reg_phases(Flags2Phases& phases) {
    // clang-format off
    assert_emplace(phases, Annex::base<compile::null>(), [](World&) { return std::unique_ptr<Phase>{}; });
    Phase::hook<compile::beta_red,         BetaRed        >(phases);
    Phase::hook<compile::branch_normalize, BranchNormalize>(phases);
    Phase::hook<compile::cleanup,          Cleanup        >(phases);
    Phase::hook<compile::eta_conv,         EtaConv        >(phases);
    Phase::hook<compile::lam_spec,         LamSpec        >(phases);
    Phase::hook<compile::named,            Named          >(phases);
    Phase::hook<compile::phases,           PhaseMan       >(phases);
    Phase::hook<compile::prefix_cleanup,   PrefixCleanup  >(phases);
    Phase::hook<compile::ret_wrap,         RetWrap        >(phases);
    Phase::hook<compile::scalarize,        Scalarize      >(phases);
    Phase::hook<compile::tail_rec_elim,    TailRecElim    >(phases);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"compile", MIM_VERSION, compile::register_normalizers, reg_phases};
}
