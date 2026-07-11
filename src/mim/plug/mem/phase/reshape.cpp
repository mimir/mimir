#include "mim/plug/mem/phase/reshape.h"

#include <mim/check.h>
#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

namespace {

bool is_mem_ty(const Def* T) { return Axm::isa<mem::M>(T); }

// TODO merge with should_flatten from tuple.*
bool should_flatten(const Def* T) {
    // handle [] cases
    if (T->isa<Sigma>()) return true;
    // also handle normalized tuple-arrays ((a:I32,b:I32) : <<2;I32>>)
    // TODO: handle better than with magic number
    //  (do we want to flatten any array with more than 2 elements?)
    //  (2 elements are needed for conditionals)
    // TODO: e.g. lea explicitely does not want to flatten

    // TODO: annotate with test cases that need these special cases
    // Problem with 2 Arr -> flatten
    // lea (2, <<2;I32>>, ...) -> lea (2, I32, I32, ...)
    if (auto lit = T->arity()->isa<Lit>(); lit && lit->get<u64>() <= 2) {
        if (auto arr = T->isa<Arr>(); arr && arr->body()->isa<Pi>()) return lit->get<u64>() > 1;
    }
    return false;
}

// TODO merge with tuple.*
DefVec flatten_ty(const Def* T) {
    DefVec types;
    if (should_flatten(T)) {
        for (auto P : T->projs()) {
            auto inner_types = flatten_ty(P);
            types.insert(types.end(), inner_types.begin(), inner_types.end());
        }
    } else {
        types.push_back(T);
    }
    return types;
}

// TODO try to remove code duplication with flatten_ty
DefVec flatten_def(const Def* def) {
    DefVec defs;
    if (should_flatten(def->type())) {
        for (auto P : def->projs()) {
            auto inner_defs = flatten_def(P);
            defs.insert(defs.end(), inner_defs.begin(), inner_defs.end());
        }
    } else {
        defs.push_back(def);
    }
    return defs;
}

} // namespace

void Reshape::apply(Mode mode) {
    mode_ = mode;
    name_ += mode == Flat ? " Flat" : " Arg";
}

void Reshape::apply(const App* app) {
    auto axm = app->arg()->as<Axm>();
    apply(axm->flags() == Annex::base<mem::reshape_arg>() ? Arg : Flat);
}

const Def* Reshape::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    auto callee = rewrite(app->callee());
    auto arg    = rewrite(app->arg());

    // Reshape normally (not to callee) to ensure that callee is reshaped correctly.
    auto reshaped_arg = reshape(arg);
    DLOG("reshape arg {} : {}", arg, arg->type());
    DLOG("into arg {} : {}", reshaped_arg, reshaped_arg->type());
    return new_world().app(callee, reshaped_arg);
}

const Def* Reshape::rewrite_mut_Lam(Lam* old_lam) {
    if (is_bootstrapping() || !old_lam->is_set()) return RWPhase::rewrite_mut_Lam(old_lam);

    auto& w = new_world();

    // main keeps its signature.
    auto is_main = *old_lam->sym() == "main";
    auto new_ty  = is_main ? rewrite(old_lam->type())->as<Pi>() : reshape_type(rewrite(old_lam->type()))->as<Pi>();

    auto new_lam = w.mut_lam(new_ty)->set(old_lam->dbg());
    if (!old_lam->is_external() && !is_main) new_lam->debug_suffix("_reshape");
    map(old_lam, new_lam);

    DLOG("Reshape lam: {} : {}", old_lam, old_lam->type());
    DLOG("         to: {} : {}", new_lam, new_ty);

    // We deeply associate `old_lam->var()` with the new var in a reconstructed shape.
    // Idea: first make the new var into an "atomic" def list, then recursively imitate `old_lam->var`.
    auto reformed_new_arg = reshape(new_lam->var(), rewrite(old_lam->var()->type()));
    DLOG("var {} : {}", old_lam->var(), old_lam->var()->type());
    DLOG("new var {} : {}", new_lam->var(), new_lam->var()->type());
    DLOG("reshaped new var {} : {}", reformed_new_arg, reformed_new_arg->type());
    map(old_lam->var(), reformed_new_arg);

    new_lam->set(rewrite(old_lam->filter()), rewrite(old_lam->body()));
    return new_lam;
}

const Def* Reshape::reshape_type(const Def* T) {
    auto& w = new_world();
    if (auto pi = T->isa<Pi>()) {
        auto new_dom = reshape_type(pi->dom());
        auto new_cod = reshape_type(pi->codom());
        return w.pi(new_dom, new_cod);
    } else if (auto sigma = T->isa<Sigma>()) {
        auto flat_types = flatten_ty(sigma);
        auto new_types  = DefVec(flat_types.size());
        std::ranges::transform(flat_types, new_types.begin(), [&](auto T) { return reshape_type(T); });
        if (mode_ == Mode::Flat) {
            const Def* mem = nullptr;
            // find mem
            for (auto i = new_types.begin(); i != new_types.end(); i++)
                if (is_mem_ty(*i) && !mem) mem = *i;
            // filter out mems
            new_types.erase(std::remove_if(new_types.begin(), new_types.end(), is_mem_ty), new_types.end());
            // readd mem in the front
            if (mem) new_types.insert(new_types.begin(), mem);
            auto reshaped_type = w.sigma(new_types);
            return reshaped_type;
        } else {
            if (new_types.size() == 0) return w.sigma();
            if (new_types.size() == 1) return new_types[0];
            const Def* mem = nullptr;
            const Def* ret = nullptr;
            // find mem
            for (auto i = new_types.begin(); i != new_types.end(); i++)
                if (is_mem_ty(*i) && !mem) mem = *i;
            // filter out mems
            new_types.erase(std::remove_if(new_types.begin(), new_types.end(), is_mem_ty), new_types.end());
            // TODO: more fine-grained test
            if (new_types.back()->isa<Pi>()) {
                ret = new_types.back();
                new_types.pop_back();
            }
            // Create the arg form `[[mem,args],ret]`
            const Def* args = w.sigma(new_types);
            if (mem) args = w.sigma({mem, args});
            if (ret) args = w.sigma({args, ret});
            return args;
        }
    } else {
        return T;
    }
}

const Def* Reshape::reshape(DefVec& defs, const Def* T, const Def* mem) {
    auto& world = T->world();
    if (should_flatten(T)) {
        auto tuples = T->projs([&](auto P) { return reshape(defs, P, mem); });
        return world.tuple(tuples);
    } else {
        const Def* def;
        if (is_mem_ty(T)) {
            assert(mem != nullptr && "Reshape: mems not found");
            def = mem;
        } else {
            do {
                assert(defs.size() > 0 && "Reshape: not enough arguments");
                def = defs.front();
                defs.erase(defs.begin());
            } while (is_mem_ty(def->type()));
        }
        // For inner function types, we override the type
        if (!def->type()->isa<Pi>()) {
            if (!Checker::alpha<Checker::Check>(def->type(), T))
                world.ELOG("reconstruct T {} from def {}", T, def->type());
            assert(Checker::alpha<Checker::Check>(def->type(), T) && "Reshape: argument type mismatch");
        }
        return def;
    }
}

const Def* Reshape::reshape(const Def* def, const Def* target) {
    DLOG("reshape:\n  {} =>\n  {}", def->type(), target);
    auto flat_defs = flatten_def(def);
    const Def* mem = nullptr;
    // find mem
    for (auto i = flat_defs.begin(); i != flat_defs.end(); i++)
        if (is_mem_ty((*i)->type()) && !mem) mem = *i;
    DLOG("mem: {}", mem);
    return reshape(flat_defs, target, mem);
}

// called for new lambda arguments, app arguments
// We can not (directly) replace it with the more general version above due to the mem erasure.
// TODO: ignore mem erase, replace with more general
// TODO: capture names
const Def* Reshape::reshape(const Def* def) {
    auto flat_defs = flatten_def(def);
    if (flat_defs.size() == 1) return flat_defs[0];
    // TODO: move mem removal to flatten_def
    if (mode_ == Mode::Flat) {
        const Def* mem = nullptr;
        // find mem
        for (auto i = flat_defs.begin(); i != flat_defs.end(); i++)
            if (is_mem_ty((*i)->type()) && !mem) mem = *i;
        // filter out mems
        flat_defs.erase(
            std::remove_if(flat_defs.begin(), flat_defs.end(), [](const Def* def) { return is_mem_ty(def->type()); }),
            flat_defs.end());
        // insert mem
        if (mem) flat_defs.insert(flat_defs.begin(), mem);
        return new_world().tuple(flat_defs);
    } else {
        // arg style
        // [[mem,args],ret]
        const Def* mem = nullptr;
        const Def* ret = nullptr;
        // find mem
        for (auto i = flat_defs.begin(); i != flat_defs.end(); i++)
            if (is_mem_ty((*i)->type()) && !mem) mem = *i;
        // filter out mems
        flat_defs.erase(
            std::remove_if(flat_defs.begin(), flat_defs.end(), [](const Def* def) { return is_mem_ty(def->type()); }),
            flat_defs.end());
        if (flat_defs.back()->type()->isa<Pi>()) {
            ret = flat_defs.back();
            flat_defs.pop_back();
        }
        const Def* args = new_world().tuple(flat_defs);
        if (mem) args = new_world().tuple({mem, args});
        if (ret) args = new_world().tuple({args, ret});
        return args;
    }
}

} // namespace mim::plug::mem::phase
