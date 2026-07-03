#include "mim/plug/affine/phase/lower_for.h"

#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/affine/affine.h"

namespace mim::plug::affine::phase {

namespace {

const Def* merge_s(const Def* elem, const Def* sigma, const Def* mem) {
    auto& w = elem->world();
    if (mem) {
        auto elems = sigma->projs();
        return cat_sigma(elem, elems);
    }
    return w.sigma({elem, sigma});
}

const Def* merge_t(const Def* elem, const Def* tuple, const Def* mem) {
    auto& w = elem->world();
    if (mem) {
        auto elems = tuple->projs();
        return cat_tuple(elem, elems);
    }
    return w.tuple({elem, tuple});
}

} // namespace

const Def* LowerFor::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    if (auto for_ax = Axm::isa<affine::For>(app)) {
        DLOG("rewriting for axm: `{}`", for_ax);
        auto [old_body, old_exit, args]               = for_ax->uncurry_args<3>();
        auto [new_begin, new_end, new_step, new_init] = args->projs<4>([this](const Def* def) { return rewrite(def); });

        auto old_body_lam = old_body->isa_mut<Lam>();
        auto old_exit_lam = old_exit->isa_mut<Lam>();
        if (!old_body_lam) old_body_lam = Lam::eta_expand(old_body);
        if (!old_exit_lam) old_exit_lam = Lam::eta_expand(old_exit);

        auto new_mem      = mem::mem_def(new_init);
        auto new_head_lam = new_world().mut_con(merge_s(new_begin->type(), new_init->type(), new_mem))->set("head");
        auto new_phis     = new_head_lam->vars();
        auto new_iter     = new_phis.front();
        auto new_acc      = new_world().tuple(new_phis.view().subspan(1));
        new_mem           = mem::mem_var(new_head_lam);
        auto new_bb_dom   = new_mem ? new_mem->type() : new_world().sigma();

        auto new_body  = new_world().mut_con(new_bb_dom)->set("new_body");
        auto new_exit  = new_world().mut_con(new_bb_dom)->set("new_exit");
        auto new_yield = new_world().mut_con(new_init->type())->set("new_yield");
        auto new_cmp   = new_world().call(core::icmp::ul, Defs{new_iter, new_end});
        auto new_inc   = new_world().call(core::wrap::add, core::Mode::nsuw, Defs{new_iter, new_step});

        new_head_lam->branch(false, new_cmp, new_body, new_exit, new_mem);
        new_yield->app(false, new_head_lam, merge_t(new_inc, new_yield->var(), new_mem));

        push();
        map(old_body_lam->var(), {new_iter, new_acc, new_yield});
        auto new_body_filter = rewrite(old_body_lam->filter());
        auto new_body_value  = rewrite(old_body_lam->body());
        new_body->set({new_body_filter, new_body_value});
        pop();

        push();
        map(old_exit_lam->var(), new_acc);
        auto new_exit_filter = rewrite(old_exit_lam->filter());
        auto new_exit_value  = rewrite(old_exit_lam->body());
        new_exit->set({new_exit_filter, new_exit_value});
        pop();

        return new_world().app(new_head_lam, merge_t(new_begin, new_init, new_mem));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::affine::phase
