#include "mim/plug/x86/phase/schedule.h"

#include <mim/plug/core/core.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;

namespace {

bool is_mem(const Def* def) {
    auto type = def->type();
    return type && Axm::isa<core::M>(type);
}

bool is_insn(const Def* def) {
    if (auto app = def->isa<App>())
        if (auto axm = app->axm())
            return axm->plugin() == Plugin_Id && !app->type()->isa<Pi>() && !Axm::isa<Addr>(app->type());
    return false;
}

} // namespace

void Schedule::place(Lam* curr, const Def* def) {
    if (def->isa_mut()) return;

    auto insn  = is_insn(def);
    auto remat = [](const Def* d) { return is_insn(d) && Scheduler::is_remat(d); };
    if (!remat(def) && !(insn ? fun_->placed : fun_->visited[curr]).emplace(def).second) return;
    if (is_mem(def)) fun_->mems[fun_->scheduler.early(def)->mut()->as<Lam>()].emplace_back(def);
    if (insn) curr = fun_->scheduler.smart(curr, def)->mut()->as<Lam>();

    for (auto op : def->ops())
        if (!remat(op)) place(curr, op);
    // Last, so that nothing separates a rematerialized operand from its user.
    for (auto op : def->ops())
        if (remat(op)) place(curr, op);
    if (insn) fun_->insns[curr].emplace_back(def);
}

const Def* Schedule::rewrite_mut_Lam(Lam* old_lam) {
    if (!old_lam->is_set() || !Lam::isa_cn(old_lam)) return RWPhase::rewrite_mut_Lam(old_lam);
    if (fun_ && fun_->nest[old_lam]) return thread(old_lam);

    // A closed Def that contains an instruction is mapped per function, so the enclosing scopes must not answer.
    auto outer = fe::Vector<Scope>();
    if (old_lam->is_closed()) {
        std::ranges::move(scopes_ | std::views::drop(1), std::back_inserter(outer));
        scopes_.resize(1);
    }

    auto fun  = Fun(old_lam);
    auto prev = std::exchange(fun_, &fun);
    for (auto mut : fun.nest.muts())
        if (auto lam = mut->isa<Lam>(); lam && lam->is_set()) place(lam, lam->body());

    auto new_lam = thread(old_lam);
    fun_         = prev;
    std::ranges::move(outer, std::back_inserter(scopes_));
    return new_lam;
}

const Def* Schedule::rewrite_imm_App(const App* app) {
    // thread() maps every instruction it places, so this one escaped all schedules.
    if (is_insn(app))
        app->blame(MIM_X86_BE "instruction `{}` is not on the `mem.M` chain of a continuation", app).bail();
    return RWPhase::rewrite_imm_App(app);
}

const Def* Schedule::thread(Lam* old_lam) {
    auto& w = new_world();
    auto n  = old_lam->num_vars();
    auto i  = n;
    for (size_t j = 0; j != n && i == n; ++j)
        if (is_mem(old_lam->var(n, j))) i = j;
    if (i == n) old_lam->blame(MIM_X86_BE "continuation `{}` has no `mem.M` to schedule on", old_lam).bail();

    auto new_lam = w.mut_lam(rewrite(old_lam->type())->as<Pi>());
    map(old_lam, new_lam);
    auto m = new_lam->var(n, i); // creates the Var the scope is keyed on
    push(Vars(old_lam->has_var()), Vars(new_lam->has_var()));

    auto order = std::move(fun_->insns[old_lam]);
    auto mems  = std::move(fun_->mems[old_lam]);

    for (auto old_def : order) {
        auto app = old_def->as<App>();
        if (Axm::isa<sched>(app)) {
            auto f   = app->callee()->as<App>()->callee()->as<App>()->arg();
            auto res = w.call<sched>(rewrite(f), m, rewrite(app->arg()));
            m        = res->proj(2, 0);
            map(old_def, res);
        } else if (Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<slot>(app) || Axm::isa<rm>(app)
                   || Axm::isa<mr>(app)) {
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
    if (n > 1) map(old_lam->var(), w.tuple(DefVec(n, [&](size_t j) { return j == i ? m : new_lam->var(n, j); })));

    auto new_filter = rewrite(old_lam->filter());
    auto new_body   = rewrite(old_lam->body());
    pop();
    return new_lam->set(new_filter, new_body);
}

} // namespace mim::plug::x86::phase
