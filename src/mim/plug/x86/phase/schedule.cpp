#include "mim/plug/x86/phase/schedule.h"

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace {

bool is_insn(const Def* def) {
    if (auto app = def->isa<App>())
        if (auto axm = app->axm())
            return axm->plugin() == Plugin_Id && !app->type()->isa<Pi>() && !Axm::isa<Addr>(app->type())
                && !Axm::isa<in>(app);
    return false;
}

/// Flags are no register value, so each consumer recomputes them right before itself.
bool is_flags(const Def* def) { return Axm::isa<Flags>(def->type()) || Axm::isa<cc>(def); }

/// An instruction without a place of its own: a fresh copy goes right before each use.
bool is_remat(const Def* def) { return is_insn(def) && (Scheduler::is_remat(def) || is_flags(def)); }

} // namespace

void Schedule::place(Lam* curr, const Def* def) {
    if (def->isa_mut()) return;

    auto insn  = is_insn(def);
    auto remat = is_remat(def);
    if (!remat && !(insn ? placed_ : visited_[curr]).emplace(def).second) return;
    if (is_mem(def)) mems_[scheduler_->early(def)->mut()->as<Lam>()].emplace_back(def);
    if (auto node = insn && !remat ? scheduler_->smart(curr, def) : nullptr) curr = node->mut()->as<Lam>();

    auto ops = DefVec(def->ops().begin(), def->ops().end());
    // A terminator branches on its condition last.
    if (auto app = def->isa<App>(); app && def == curr->body()) ops = {app->arg(), app->callee()};
    for (auto op : ops)
        if (!is_remat(op)) place(curr, op);
    // Last, so that nothing separates a rematerialized operand from its user.
    for (auto op : ops)
        if (is_remat(op)) place(curr, op);
    if (insn) insns_[curr].emplace_back(def);
}

void Schedule::visit(const Nest& nest) {
    auto scheduler = Scheduler(nest);
    scheduler_     = &scheduler;
    reset();
    placed_.clear(), threaded_.clear(), visited_.clear(), insns_.clear(), mems_.clear();

    for (auto mut : nest.muts())
        if (auto lam = mut->isa<Lam>(); lam && lam->is_set()) place(lam, lam->body());
    rewrite(root());
}

const Def* Schedule::rewrite(const Def* old_def) {
    if (auto mut = old_def->isa_mut()) {
        if (auto lam = mut->isa<Lam>(); lam && lam->is_set() && nest()[lam] && threaded_.emplace(lam).second)
            thread(lam);
        return mut;
    }
    return Rewriter::rewrite(old_def);
}

const Def* Schedule::rewrite_imm_App(const App* app) {
    // thread() maps every instruction it places, so this one escaped all schedules.
    if (is_insn(app))
        app->blame(MIM_X86_BE "instruction `{}` is not on the `mem.M` chain of a continuation", app).bail();
    return Rewriter::rewrite_imm_App(app);
}

void Schedule::thread(Lam* lam) {
    auto n = lam->num_vars();
    auto i = n;
    for (size_t j = 0; j != n && i == n; ++j)
        if (is_mem(lam->var(n, j))) i = j;

    // Direct style has no chain, so an instruction in there runs into rewrite_imm_App.
    if (i == n && Lam::isa_cn(lam))
        lam->blame(MIM_X86_BE "continuation `{}` has no `mem.M` to schedule on", lam).bail();

    push(Vars(lam->has_var()), Vars(lam->has_var()));
    if (i != n) chain(lam, i);
    auto filter = rewrite(lam->filter());
    auto body   = rewrite(lam->body());
    pop();
    lam->unset()->set(filter, body);
}

void Schedule::chain(Lam* lam, size_t i) {
    auto& w    = world();
    auto n     = lam->num_vars();
    auto m     = lam->var(n, i);
    auto order = std::move(insns_[lam]);
    auto mems  = std::move(mems_[lam]);

    // Instructions hoisted into lam come after the condition of its branch, so the branch gets a fresh one at the end.
    if (auto ex = lam->body()->as<App>()->callee()->isa<Extract>(); ex && is_remat(ex->index())) {
        auto cc = ex->index()->as<App>();
        if (is_remat(cc->arg())) order.emplace_back(cc->arg());
        order.emplace_back(cc);
    }

    for (auto old_def : order) {
        auto app = old_def->as<App>();
        if (Axm::isa<sched>(app)) {
            auto res = w.call<sched>(rewrite(sched_insn(app)), m, rewrite(app->arg()));
            m        = res->proj(2, 0);
            map(old_def, res);
        } else if (prev_mem(app)) {
            auto old_arg = app->arg();
            auto k       = old_arg->num_projs();
            auto args    = DefVec(k, [&](size_t j) {
                auto a = old_arg->proj(k, j);
                return is_mem(a) ? m : rewrite(a);
            });
            auto new_app = w.app(rewrite(app->callee()), w.tuple(args));
            if (is_mem(new_app)) {
                m = new_app;
            } else {
                m = new_app->proj(2, 0);
                map(old_def, new_app);
            }
        } else {
            auto res = w.call<sched>(rewrite(app->callee()), m, rewrite(app->arg()));
            m        = res->proj(2, 0);
            map(old_def, res->proj(2, 1));
        }
    }

    for (auto old_mem : mems)
        map(old_mem, m);
    // The whole Var may be passed on, so it must carry the end of the chain as well.
    if (n > 1) map(lam->var(), w.tuple(DefVec(n, [&](size_t j) { return j == i ? m : lam->var(n, j); })));
}

} // namespace mim::plug::x86::phase
