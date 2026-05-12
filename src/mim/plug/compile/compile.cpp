#include "mim/plug/compile/compile.h"

#include <memory>

#include <mim/config.h>
#include <mim/driver.h>
#include <mim/pass.h>
#include <mim/phase.h>

#include <mim/pass/beta_red.h>
#include <mim/pass/eta_exp.h>
#include <mim/pass/eta_red.h>
#include <mim/pass/lam_spec.h>
#include <mim/pass/scalarize.h>
#include <mim/pass/tail_rec_elim.h>
#include <mim/phase/beta_red_phase.h>
#include <mim/phase/branch_normalize.h>
#include <mim/phase/eta_exp_phase.h>
#include <mim/phase/eta_red_phase.h>
#include <mim/phase/prefix_cleanup.h>
#include <mim/phase/ret_wrap.h>
#include <mim/phase/sym_expr_opt.h>

#include "mim/plug/compile/autogen.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    // clang-format off
    assert_emplace(stages, Annex::base<compile::null_phase>(), [](World&) { return std::unique_ptr<Phase>{}; });
    assert_emplace(stages, Annex::base<compile::null_repl >(), [](World&) { return std::unique_ptr<Repl >{}; });
    assert_emplace(stages, Annex::base<compile::null_pass >(), [](World&) { return std::unique_ptr<Pass >{}; });
    // phases
    Stage::hook<compile::beta_red_phase,         BetaRedPhase        >(stages);
    Stage::hook<compile::branch_normalize_phase, BranchNormalizePhase>(stages);
    Stage::hook<compile::cleanup_phase,          Cleanup             >(stages);
    Stage::hook<compile::eta_exp_phase,          EtaExpPhase         >(stages);
    Stage::hook<compile::eta_red_phase,          EtaRedPhase         >(stages);
    Stage::hook<compile::pass2phase,             PassManPhase        >(stages);
    Stage::hook<compile::repl2phase,             ReplManPhase        >(stages);
    Stage::hook<compile::sym_expr_opt,           SymExprOpt          >(stages);
    Stage::hook<compile::phases,                 PhaseMan            >(stages);
    Stage::hook<compile::prefix_cleanup_phase,   PrefixCleanup       >(stages);
    // repls
    Stage::hook<compile::repls,                  ReplMan             >(stages);
    // passes
    Stage::hook<compile::beta_red_pass,          BetaRed             >(stages);
    Stage::hook<compile::eta_exp_pass,           EtaExp              >(stages);
    Stage::hook<compile::eta_red_pass,           EtaRed              >(stages);
    Stage::hook<compile::lam_spec_pass,          LamSpec             >(stages);
    Stage::hook<compile::passes,                 PassMan             >(stages);
    Stage::hook<compile::ret_wrap_phase,         RetWrap             >(stages);
    Stage::hook<compile::scalarize_pass,         Scalarize           >(stages);
    Stage::hook<compile::tail_rec_elim_pass,     TailRecElim         >(stages);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"compile", MIM_VERSION, compile::register_normalizers, reg_stages, nullptr};
}
