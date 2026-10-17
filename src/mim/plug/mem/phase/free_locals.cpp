#include "mim/plug/mem/phase/free_locals.h"

namespace mim::plug::mem::phase {

bool FreeLocals::analyze() {
    if (RWPhase::analyze()) return true;

    const auto nest = Nest(old_world());
    mentions(nest.root());

    for (const auto& [lam, chain] : escape_.chains()) {
        auto last = DefMap<size_t>();
        for (size_t i = 0, n = chain.ops.size(); i != n; ++i) {
            last[chain.ops[i]] = i;
            for (auto alloc : chain.uses[i])
                last[alloc] = i;
        }

        for (auto alloc : chain.locals) {
            if (escape_.confined(alloc)) {
                auto i = last[alloc] + 1;
                frees_[i < chain.ops.size() ? chain.ops[i] : chain.tail].emplace_back(alloc);
            } else {
                place(nest[lam], nest[lam], alloc);
            }
        }
    }

    log().d("free_locals: frees before {} ops, on {} edges", frees_.size(), edges_.size());
    return false;
}

void FreeLocals::mentions(const Nest::Node* node) {
    auto& res = mentions_[node];
    if (auto lam = node->mut() ? node->mut()->isa_mut<Lam>() : nullptr)
        if (auto i = escape_.chains().find(lam); i != escape_.chains().end())
            for (const auto& uses : i->second.uses)
                res.insert(uses.begin(), uses.end());
    for (auto child : node->children().nodes()) {
        mentions(child);
        const auto& sub = mentions_[child];
        mentions_[node].insert(sub.begin(), sub.end());
    }
}

void FreeLocals::place(const Nest::Node* home, const Nest::Node* node, const Def* alloc) {
    for (auto child : node->children().nodes())
        place(home, child, alloc);

    auto lam = node->mut()->isa_mut<Lam>();
    if (!lam || escape_.escaping(lam) || (node != home && !live(home, node, alloc))) return;
    auto i = escape_.chains().find(lam);
    if (i == escape_.chains().end()) return;

    const auto& chain = i->second;
    auto deads        = DefVec();
    for (auto target : chain.targets)
        if (dead(home, target, alloc)) deads.emplace_back(target);

    if (deads.size() == chain.targets.size())
        frees_[chain.tail].emplace_back(alloc);
    else
        for (auto target : deads)
            edges_[chain.tail][target].emplace_back(alloc);
}

bool FreeLocals::live(const Nest::Node* home, const Nest::Node* node, const Def* alloc) {
    if (mentions_[node].contains(alloc)) return true;
    for (auto x = node; x != home; x = x->inest()) {
        auto todo = fe::Vector<const Nest::Node*>(x->sibl_deps().nodes().begin(), x->sibl_deps().nodes().end());
        auto done = std::set<const Nest::Node*>();
        while (!todo.empty()) {
            auto s = todo.back();
            todo.pop_back();
            if (!done.emplace(s).second) continue;
            if (mentions_[s].contains(alloc)) return true;
            for (auto d : s->sibl_deps().nodes())
                todo.emplace_back(d);
        }
    }
    return false;
}

bool FreeLocals::dead(const Nest::Node* home, const Def* target, const Def* alloc) {
    auto scope = home->mut();
    if (!scope->nests(target)) return true;
    if (auto lam = target->isa_mut<Lam>()) return !live(home, home->nest()[lam], alloc);
    return false;
}

const Def* FreeLocals::rewrite_imm_App(const App* app) {
    auto allocs = fe::lookup(frees_, app);
    auto edges  = fe::lookup(edges_, app);
    if (!allocs && !edges) return RWPhase::rewrite_imm_App(app);

    auto& w      = new_world();
    auto new_arg = rewrite(app->arg());
    if (allocs) {
        if (auto mem = mem_def(app->arg()))
            new_arg = thread(app->arg(), mem, *allocs);
        else
            log().d("free_locals: `{}` threads no memory, leaking {} allocations", app, allocs->size());
    }

    if (!edges) return w.app(rewrite(app->callee()), new_arg);
    auto args = DefVec(app->num_targs(), [&](size_t i) {
        auto arg = app->targ(i);
        return Pi::isa_cn(arg->type()) ? rewrite_target(arg, *edges) : new_arg->proj(app->num_targs(), i);
    });
    return w.app(rewrite_target(app->callee(), *edges), w.tuple(args));
}

const Def* FreeLocals::thread(const Def* arg, const Def* mem, const DefVec& allocs) {
    auto& w = new_world();
    if (arg == mem) {
        auto new_mem = rewrite(mem);
        for (auto alloc : allocs)
            new_mem = w.call<mem::free>(Defs{new_mem, rewrite(alloc->proj(2, 1))});
        return new_mem;
    }
    if (mem_def(arg) != mem) return rewrite(arg);
    auto n = arg->num_projs();
    return w.tuple(DefVec(n, [&](size_t i) { return thread(arg->proj(n, i), mem, allocs); }));
}

const Def* FreeLocals::rewrite_target(const Def* target, const DefMap<DefVec>& edges) {
    auto& w = new_world();
    if (auto allocs = fe::lookup(edges, target)) return trampoline(rewrite(target), *allocs);
    if (auto tuple = target->isa<Tuple>())
        return w.tuple(DefVec(tuple->num_ops(), [&](size_t i) { return rewrite_target(tuple->op(i), edges); }));
    if (auto sw = Switch(target)) return w.extract(rewrite_target(sw.tuple(), edges), rewrite(sw.index()));
    return rewrite(target);
}

const Def* FreeLocals::trampoline(const Def* cont, const DefVec& allocs) {
    auto& w   = new_world();
    auto lam  = w.mut_con(Pi::isa_cn(cont->type())->dom())->set("free_then");
    auto n    = lam->num_vars();
    auto args = DefVec(n, [&](size_t i) { return lam->var(n, i); });
    auto mem  = std::ranges::find_if(args, [](const Def* arg) { return isa_mem(arg); });
    if (mem == args.end()) {
        log().d("free_locals: `{}` takes no memory, leaking {} allocations", cont, allocs.size());
        return cont;
    }
    for (auto alloc : allocs)
        *mem = w.call<mem::free>(Defs{*mem, rewrite(alloc->proj(2, 1))});
    lam->app(false, cont, args);
    return lam;
}

} // namespace mim::plug::mem::phase
