#include <absl/container/flat_hash_set.h>

#include "mim/lam.h"
#include "mim/world.h"

#include "mim/plug/scf/autogen.h"

namespace mim::plug::scf {

namespace {

/// Peels one layer off a `Path` built from `%scf.arm`/`%scf.case`/`%scf.default`/
/// `%scf.body`, returning the immediate parent `Path`, or `nullptr` if `p` is not
/// one of those four literal constructor applications (i.e. peeling is stuck --
/// typically a function's own bound `path` parameter).
const Def* peel_path(const Def* p) {
    if (Axm::isa<arm>(p)) return App::uncurry_args<3>(p)[1];      // Bool, Path, Step -> Path
    if (Axm::isa<_case>(p)) return App::uncurry_args<3>(p)[1];    // Nat,  Path, Step -> Path
    if (Axm::isa<_default>(p)) return App::uncurry_args<2>(p)[0]; // Path, Step       -> Path
    if (Axm::isa<body>(p)) return App::uncurry_args<2>(p)[0];     // Path, Step       -> Path
    return nullptr;
}

/// Is `candidate_path` strictly nested inside `root_path`, the coordinate of
/// the If/Switch/Loop capability a break/continue is being fired against?
/// Interim stand-in for real linearity (not enforced pre-QTT): rejects a
/// capability smuggled out of its construct as ordinary data and
/// replayed from a point that never structurally descends from the
/// construct's own path -- including that construct's own post-exit
/// continuation, which sits AT `root_path` (zero peels), not inside it.
/// Equal paths are never legitimate here: every real break/continue site is
/// reached through at least one `%scf.body`/`%scf.case`/etc. layer, so
/// requiring strict nesting costs nothing and needs no `Step` comparison.
bool path_contains(const Def* root_path, const Def* candidate_path) {
    for (auto p = peel_path(candidate_path); p; p = peel_path(p))
        if (p == root_path) return true;
    return false;
}

/// `%scf.continue`/`%scf.break(l/s)`/`%scf.switch` all end in a bare `Cn X`
/// result, so with no explicit curry override their normalizer only fires
/// once that final `Cn X` is itself applied to its payload (matching
/// `%scf.if`/`%scf.merge`, which have no override either) -- one application
/// further than these normalizers historically expected. `callee` is
/// therefore the App one curry step short of the *payload*, i.e. the step
/// that applied the Loop/Switch capability; its own arg is that capability,
/// and peeling one more level down gives the step that applied `inner_token`.
const Def* inner_path(const Def* callee) {
    auto inner_token                = callee->as<App>()->callee()->as<App>()->arg();
    auto [inner_path_, inner_step_] = App::uncurry_args<2>(inner_token->type());
    return inner_path_;
}

} // namespace

const Def* normalize_switch(const Def*, const Def* callee, const Def*) {
    // `callee` has token/break/default/cases/index all applied (see the
    // `inner_path` comment above for why); `cases` is one step back from
    // `index`.
    auto cases = callee->as<App>()->callee()->as<App>()->arg();
    absl::flat_hash_set<nat_t> labels;
    for (auto c : cases->projs()) {
        auto index = c->proj(2, 0);
        if (auto l = Lit::isa(index))
            if (!labels.insert(*l).second) mim::error(c->loc(), "duplicate case label '{}' in switch", *l);
    }

    return {};
}

const Def* normalize_continue(const Def*, const Def* callee, const Def*) {
    auto tok                    = inner_path(callee);
    auto capability             = callee->as<App>()->arg();
    auto [path, step_, B_, H_]  = App::uncurry_args<4>(capability->type());

    if (!path_contains(path, tok))
        mim::error(tok->loc(), "token passed to '%scf.continue' does not belong to the targeted loop");

    return {};
}

template<_break id>
const Def* normalize_break(const Def*, const Def* callee, const Def*) {
    auto tok        = inner_path(callee);
    auto capability = callee->as<App>()->arg();
    const Def* path;

    if constexpr (id == _break::l) {
        auto [p, step_, B_, H_] = App::uncurry_args<4>(capability->type());
        path                    = p;
    } else {
        auto [p, step_, T_, B_] = App::uncurry_args<4>(capability->type());
        path                    = p;
    }

    if (!path_contains(path, tok))
        mim::error(tok->loc(), "token passed to '{}' does not belong to the targeted {}",
                   id == _break::l ? "%scf.break.l" : "%scf.break.s", id == _break::l ? "loop" : "switch");

    return {};
}

MIM_scf_NORMALIZER_IMPL

} // namespace mim::plug::scf
