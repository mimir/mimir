#include "dumper.h"

using namespace std::literals;

namespace mim::dump {

namespace {

std::string_view external(const Def* def) {
    if (def->is_external()) return "extern "sv;
    return ""sv;
}

} // namespace

void Dumper::dump_annexes(std::string self, std::span<const DumpAnnex> annexes) {
    p_.namer().self(std::move(self));
    auto content = [](const Def* def) { return def->isa<Axm>() ? def->type() : def->as<Nom>()->op(); };
    auto muts    = fe::Vector<Def*>();
    for (const auto& [_, __, annex] : annexes)
        for (auto mut : content(annex)->local_muts())
            if (isa_decl(mut)) muts.emplace_back(mut);
    dump_muts(muts);

    sep(Prev::Decl);
    auto noms   = DefVec();
    auto names  = DefMap<std::string_view>();
    auto bodies = fe::Vector<Def*>();
    for (const auto& a : annexes)
        if (auto nom = a.def->isa<Nom>()) {
            noms.emplace_back(nom);
            names[nom] = a.name;
            for (auto mut : nom->local_muts())
                bodies.emplace_back(mut);
        }
    if (!bodies.empty()) group_noms(Nest(bodies.front()->world(), bodies, true), noms);
    std::string_view curr_mod;
    for (const auto& [mod, name, def] : annexes) {
        if (mod != curr_mod) {
            if (!curr_mod.empty()) std::println(os_, "{}end", --p_.tab());
            if (!mod.empty()) std::println(os_, "{}mod {}", p_.tab(), mod), ++p_.tab();
            curr_mod = mod;
            p_.namer().mod(mod.empty() ? std::string() : std::string(mod) + ".");
        }
        if (deferred_.contains(def)) continue;
        if (auto group = fe::lookup(mutual_, def)) {
            emit_mutual(*group, [&](const Def* m) { emit_nom("anx nom", names[m], m->as<Nom>()); });
            continue;
        }
        if (auto nom = def->isa<Nom>()) {
            emit_nom("anx nom", name, nom);
            continue;
        }
        auto axm = def->as<Axm>();
        std::print(os_, "{}axm {}: {}", p_.tab(), name, p_.op(axm->type()));
        auto [curry, trip] = Axm::infer_curry_and_trip(axm->type());
        if (axm->curry() != curry || axm->trip() != trip) std::print(os_, ", {}", axm->curry());
        if (axm->trip() != trip) std::print(os_, ", {}", axm->trip());
        std::println(os_, ";");
    }
    if (!curr_mod.empty()) std::println(os_, "{}end", --p_.tab());
    p_.namer().mod({});
}

void Dumper::dump(const Def* def) {
    if (auto mut = isa_decl(def)) return dump_muts({mut});

    auto muts = fe::Vector<Def*>();
    for (auto mut : def->local_muts())
        if (isa_decl(mut)) muts.emplace_back(mut);
    dump_muts(muts, def);
}

void Dumper::dump_muts(fe::View<Def*> muts, const Def* tail) {
    if (muts.empty() && !tail) return;
    auto todo = fe::Vector<Def*>();
    for (auto mut : muts)
        post_order(mut, todo);
    for (auto [lam, match] : arm_of_)
        if (match && is_spellable(lam)) plan().arms.emplace(lam);

    // Nest::Node::topo_ breaks ties by gid, which a re-read dump need not reproduce.
    auto roots = fe::Vector<Def*>();
    for (auto mut : todo) {
        if (!mut->is_closed() || plan().arms.contains(mut)) continue;
        if (is_imported(mut))
            done_.emplace(mut);
        else
            roots.emplace_back(mut);
    }
    if (mode_ == Def::Dump::Scope) scope_.insert(muts.begin(), muts.end());

    const auto nest = Nest(tail ? tail->world() : muts.front()->world(), muts, true);
    auto _          = fe::Restore(nest_, &nest);
    emit_top(roots);
    if (tail) {
        emit_top(std::array{tail});
        sep(Prev::Let);
        emit_tail(tail, "");
    }
}

bool Dumper::is_spellable(const Def* arm) {
    auto lam = arm->isa_mut<Lam>();
    return lam && lam->is_set() && lam->filter() == lam->world().lit_tt();
}

void Dumper::post_order(Def* mut, fe::Vector<Def*>& todo) {
    if (!scheduled_.emplace(mut).second) return;

    auto muts = fe::Vector<Def*>();
    auto seen = DefSet();
    printed_deps(mut, [&](const Def* op) {
        note_use(op, nullptr);
        local_muts(op, seen, muts);
    });
    for (auto local_mut : muts)
        post_order(local_mut, todo);

    todo.emplace_back(mut);
}

void Dumper::local_muts(const Def* def, DefSet& seen, fe::Vector<Def*>& muts) {
    if (!def || def->local_muts().empty() || !seen.emplace(def).second) return;
    if (auto mut = def->isa_mut()) return (void)muts.emplace_back(mut);
    auto match = def->isa<Match>();
    printed_deps(def, [&](const Def* op) {
        // An arm twice in the same `match` cannot be spelled out at both places.
        auto arm = match && std::ranges::count(match->arms(), op) == 1;
        note_use(op, arm ? match : nullptr);
        local_muts(op, seen, muts);
    });
}

void Dumper::note_use(const Def* op, const Match* match) {
    if (!op || !op->isa_mut<Lam>()) return;
    if (auto [i, ins] = arm_of_.emplace(op, match); !ins && !match) i->second = nullptr;
}

void Dumper::note_vars(const Def* def) {
    if (p_.isa_inst(def)) {
        for (auto arg : def->as<Nom>()->args())
            if (arg->isa<Var>()) plan().whole_vars.emplace(arg);
        return;
    }
    auto ex = def->isa<Extract>();
    for (auto op : def->ops())
        if (op && op->isa<Var>() && !(ex && op == ex->tuple() && ex->index()->isa<Lit>()))
            plan().whole_vars.emplace(op);
}

void Dumper::schedule_(const Def* def, bool let) {
    if (!def) return;
    if (auto mut = isa_decl(def); mut && !plan().arms.contains(mut)) return schedule_mut(mut);
    if (!done_.emplace(def).second) return finish(def);
    if (plan().arms.contains(def)) {
        // An arm has no block of its own either.
        auto lam = def->as_mut<Lam>();
        if (lam->body()->isa<Var>()) plan().whole_vars.emplace(lam->body());
        schedule_inline(lam->dom(), plan().inlined);
        schedule_inline(lam->body(), plan().inlined);
        return;
    }
    note_vars(def);
    schedule_deps(def);
    if ((let || p_.has_decl(def)) && !p_.is_full(def)) order_.emplace_back(def);
}

void Dumper::schedule_deps(const Def* def) {
    pending_.emplace(def);
    printed_deps(def, [&](const Def* op) { schedule_(op); });
    pending_.erase(def);
}

void Dumper::finish(const Def* def) {
    if (pending_.erase(def)) printed_deps(def, [&](const Def* op) { schedule_(op); });
}

void Dumper::schedule_mut(Def* mut) {
    if (!enter(mut) || done_.contains(mut)) return;

    auto group = sibling_group(mut);
    for (auto m : group)
        done_.emplace(m);
    for (auto m : group)
        schedule_body(m);
    order_.emplace_back(group.front());
    if (group.size() > 1) mutual_.emplace(group.front(), DefVec(group.begin(), group.end()));
}

fe::Vector<Def*> Dumper::sibling_group(Def* mut) const {
    auto node = (*nest_)[mut];
    if (!node || !node->is_mutually_recursive()) return {mut};
    if (auto key = owner(node->inest()); key && inlines(key)) return {mut};

    auto group = fe::Vector<Def*>();
    for (auto member : node->scc())
        if (auto m = member->mut(); m == mut || !done_.contains(m)) group.emplace_back(m);
    if (group.size() < 2 || !std::ranges::all_of(group, [this](Def* m) { return groupable(m); })) return {mut};
    return group;
}

bool Dumper::groupable(Def* mut) const {
    if (!isa_decl(mut) || plan().arms.contains(mut) || mut->isa<Rule>() || !mut->is_set()) return false;
    if (auto lam = mut->isa_mut<Lam>()) return curry_chain(lam).back()->is_set();
    return true;
}

void Dumper::schedule_body(Def* mut) {
    auto chain = mut->isa_mut<Lam>() ? curry_chain(mut->as_mut<Lam>()) : fe::Vector<Lam*>();

    if (auto rule = mut->isa_mut<Rule>()) {
        // The surface syntax has no block for a rule's parts, so whatever depends on its meta variable is inlined.
        printed_deps(rule->type(), [&](const Def* op) { schedule_inline(op, plan().inlined); });
        printed_deps(rule, [&](const Def* op) { schedule_inline(op, plan().inlined); });
    } else if (chain.empty()) {
        for (auto op : mut->deps())
            schedule_(op);
    } else {
        for (auto* lam : chain)
            if (lam != mut) done_.emplace(lam), absorbed_.emplace(lam, mut);
        // The chain spells out its Pi%s, so only their components are referenced.
        for (auto* lam : chain) {
            schedule_inline(lam->type()->dom(), plan().header);
            if (auto var = lam->has_var()) schedule_proj_types(var);
            schedule_inline(lam->filter(), plan().header);
            if (lam == chain.back()) {
                schedule_inline(lam->type()->codom(), plan().header);
                schedule_tail(lam->body());
            }
        }
    }
}

void Dumper::schedule_proj_types(const Def* def) {
    auto n = def->num_tprojs();
    if (n <= 1) return;
    for (size_t i = 0; i != n; ++i)
        if (auto proj = def->proj(n, i)) {
            schedule_inline(proj->type(), plan().header);
            schedule_proj_types(proj);
        }
}

void Dumper::schedule_inline(const Def* def, DefSet& inline_set) {
    if (!def || def->is_closed() || p_.has_decl(def)) return schedule_(def);
    if (!inline_set.emplace(def).second) return;
    note_vars(def);
    printed_deps(def, [&](const Def* op) { schedule_inline(op, inline_set); });
}

void Dumper::schedule_tail(const Def* def) {
    if (def && def->isa<Var>()) plan().whole_vars.emplace(def);
    schedule_(def, false);
}

bool Dumper::enter(Def* mut) const {
    auto node = (*nest_)[mut];
    if (!node || mode_ != Def::Dump::Scope) return node != nullptr;
    while (!node->inest()->is_root())
        node = node->inest();
    return scope_.contains(node->mut());
}

void Dumper::bucket() {
    for (auto def : order_) {
        auto mut = isa_decl(def);
        auto key = owner(nest_->inest(def));
        // A Nom is the one immutable with a declaration of its own, so it never inlines.
        if (def->isa<Nom>()) {
            while (key && inlines(key))
                key = owner((*nest_)[key]->inest());
            // TODO A Nom over a Var in its own Lam's signature has no valid place: `lam k (n: Nat, x: V n)`.
        }
        // A binder that prints inline has no block of its own, so what belongs into it is inlined as well.
        if (key && inlines(key) && !(mut && is_recursive(mut))) {
            plan().inlined.emplace(def);
            continue;
        }
        (key ? bucket_[key] : top_).emplace_back(def);
    }
    order_.clear();
}

bool Dumper::inlines(Def* mut) const {
    for (auto node = (*nest_)[mut]; node; node = node->inest())
        if (auto m = owner(node); m && (!isa_decl(m) || plan().arms.contains(m))) return true;
    return false;
}

Def* Dumper::owner(const Nest::Node* node) const {
    if (!node) return nullptr;
    auto mut = node->mut();
    if (!mut) return nullptr;                                  // a *virtual* root is the top level
    if (auto outer = fe::lookup(absorbed_, mut)) return outer; // a curried chain shares a block
    return mut;
}

void Dumper::emit(Def* key) {
    auto defs = key ? std::move(bucket_[key]) : std::move(top_);
    group_noms(*nest_, defs);
    for (auto def : defs) {
        if (deferred_.contains(def)) continue;
        auto group = fe::lookup(mutual_, def);
        sep(group || isa_decl(def) ? Prev::Decl : Prev::Let);
        if (group)
            emit_mutual(*group, [this](const Def* m) { emit_item(m); });
        else
            emit_item(def);
    }
}

void Dumper::emit_item(const Def* def) {
    if (auto mut = isa_decl(def)) return emit_decl(mut);
    emit_let(def);
}

void Dumper::emit_nom(std::string_view kw, std::string_view name, const Nom* nom) {
    std::print(os_, "{}{} {} = ", p_.tab(), kw, name);
    emit_rhs(p_.op(nom->op()));
}

void Dumper::emit_rhs(Op rhs) {
    if (auto variant = rhs->isa<Variant>())
        p_.ctors(os_, variant);
    else
        os_ << rhs;
    os_ << ";\n";
}

void Dumper::group_noms(const Nest& nest, const DefVec& defs) {
    auto grouped = DefSet();
    for (auto def : defs) {
        auto nom = def->isa<Nom>();
        if (!nom || grouped.contains(nom)) continue;
        for (auto mut : nom->local_muts()) {
            auto node = nest[mut];
            if (!node || !node->is_recursive()) continue;
            auto on_cycle = scc_noms(nest, node->scc());
            auto cycle    = DefVec();
            for (auto d : defs)
                if (on_cycle.contains(d)) cycle.emplace_back(d);
            if (cycle.size() > 1) {
                grouped.insert(cycle.begin(), cycle.end());
                deferred_.insert(cycle.begin(), cycle.end() - 1);
                mutual_.emplace(cycle.back(), std::move(cycle));
            }
            break;
        }
    }
}

DefSet Dumper::scc_noms(const Nest& nest, const Nest::Node::SCC& scc) {
    auto reaches_scc = [&](const Def* nom) {
        return std::ranges::any_of(nom->local_muts(), [&](Def* mut) { return scc.contains(nest[mut]); });
    };
    auto res  = DefSet();
    auto todo = DefVec();
    auto seen = DefSet();
    for (auto node : scc)
        for (auto dep : node->mut()->deps())
            if (dep && seen.emplace(dep).second) todo.emplace_back(dep);
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if (def->isa_mut()) continue;
        if (def->isa<Nom>() && reaches_scc(def)) res.emplace(def);
        for (auto dep : def->deps())
            if (dep && seen.emplace(dep).second) todo.emplace_back(dep);
    }
    return res;
}

void Dumper::sep(Prev curr) {
    if (prev_ == Prev::Decl || (prev_ == Prev::Let && curr == Prev::Decl)) os_ << '\n';
    prev_ = curr;
}

void Dumper::emit_let(const Def* def) {
    if (auto nom = def->isa<Nom>()) return emit_nom("nom", p_.name(nom), nom);
    std::print(os_, "{}let {}", p_.tab(), p_.name(def));
    if (typed_let_) std::print(os_, ": {}", p_.op(def->type()));
    std::println(os_, " = {};", p_.full(def));
}

void Dumper::emit_decl(Def* mut) {
    if (auto lam = mut->isa_mut<Lam>()) return emit_lam(lam);
    if (auto rule = mut->isa_mut<Rule>(); rule && rule->is_set()) return emit_rule(rule);
    if (!mut->is_set()) return emit_unset(mut);
    // `rec` binds the name for the body - which only a self-referential mutable needs; `extern` is out either way.
    std::print(os_, "{}{}{} = ", p_.tab(), is_recursive(mut) ? "rec " : "let ", p_.id(mut));
    emit_rhs(p_.full(mut));
}

void Dumper::emit_rule(Rule* rule) {
    auto dom = rule->dom();
    auto num = num_binders(dom);
    std::print(os_, "{}rule {} ", p_.tab(), p_.id(rule));
    auto var = rule->has_var();
    p_.bndr(os_, Delim::ptrn(), var, dom, num, num, num != 0 && p_.split(var, dom, num), false);
    std::print(os_, ": {}", p_.full(rule->lhs()));
    if (rule->guard() != rule->world().lit_tt()) std::print(os_, " when {}", p_.full(rule->guard()));
    std::println(os_, " => {};", p_.full(rule->rhs()));
}

void Dumper::emit_unset(Def* mut) {
    std::println(os_, "{}// `{}: {}` is unset", p_.tab(), p_.id(mut), p_.op(mut->type()));
}

void Dumper::emit_lam(Lam* lam) {
    auto chain = curry_chain(lam);
    auto last  = chain.back();
    auto kind  = lam_kind(last, p_.isa_fun(last) && !shadows_ret(last));

    // A `fun` binds its `ret` continuation as `return`, so that is the name its Var has to print with.
    if (kind == LamKind::Fun) {
        if (auto var = last->has_var()) {
            if (auto num = num_binders(last->type()->dom()); num == 1)
                p_.namer().bind(var, "return");
            else
                p_.namer().ret(var, num - 1);
        }
    }
    // A Lam without a body forward-declares what a native translation unit provides - and only an `extern`
    // may go without one.
    if (!last->is_set()) return emit_bodyless(lam, last, kind);

    // An inner Lam of the chain has no name; its outer Lam%s applied to their own Var%s reduce to it.
    auto ref = p_.id(lam);
    for (size_t i = 1; i != chain.size(); ++i) {
        auto var = chain[i - 1]->has_var();
        if (!var) break;
        ref += std::format(" {}{}", chain[i - 1]->type()->is_implicit() ? "@" : "", p_.name(var));
        p_.namer().bind(chain[i], std::format("({})", ref));
    }

    std::print(os_, "{}{}{} {}", p_.tab(), external(lam), decl_kw(kind), p_.id(lam));
    p_.lam_header(os_, chain, kind);
    os_ << " =\n";

    ++p_.tab();
    rets_.emplace_back(kind == LamKind::Fun ? last->has_var() : nullptr);
    auto _ = fe::Restore(prev_, Prev::None);
    emit(lam);
    sep(Prev::Let);
    emit_tail(last->body(), ";");
    rets_.pop_back();
    --p_.tab();
}

void Dumper::emit_bodyless(Lam* lam, Lam* last, LamKind kind) {
    auto dom = last->type()->dom();
    std::print(os_, "{}extern {} {}", p_.tab(), decl_kw(kind), p_.id(lam));
    auto num = num_binders(dom);
    if (kind == LamKind::Fun) return std::println(os_, "[{}]: {};", p_.op(dom->proj(num, 0)), p_.op(last->ret_dom()));

    // Flattened, the components of a dependent domain would refer to binders nothing prints.
    if (auto [_, var] = dom->isa_binder<Sigma>(); var)
        os_ << p_.op(dom);
    else
        std::print(os_, "[{}]", p_.ops(dom->projs(num)));
    if (kind == LamKind::Lam) std::print(os_, ": {}", p_.op(last->type()->codom()));
    os_ << ";\n";
}

void Dumper::emit_tail(const Def* def, std::string_view end) {
    std::println(os_, "{}{}{}", p_.tab(), p_.tail(def), end);
}

bool Dumper::shadows_ret(Lam* lam) const {
    for (auto* var : rets_)
        if (var && lam->has_free_var(var)) return true;
    return false;
}

} // namespace mim::dump
