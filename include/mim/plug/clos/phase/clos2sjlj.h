#pragma once

#include <mim/phase.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

/// Lowers basicblock closures that are passed as arguments (i.e. exception continuations)
/// to setjmp/longjmp: the caller setjmps and dispatches on the tag, the closures become longjmps.
class Clos2SJLJ : public RWPhase {
public:
    Clos2SJLJ(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite(const Def*) final;
    /// Restructures @p filter and @p body of @p lam if @p body passes exception closures.
    std::pair<const Def*, const Def*> finish_Lam(Lam* lam, const Def* filter, const Def* body) final;

    const Def* void_ptr() { return new_world().annex<clos::BufPtr>(); }
    const Def* jb_type() { return void_ptr(); }
    const Def* rb_type() { return new_world().call<mem::Ptr0>(void_ptr()); }
    const Def* tag_type() { return new_world().type_i32(); }

    Lam* get_throw(const Def* res_type);
    Lam* get_lpad(Lam* lam, const Def* rb);

    void get_exn_closures(const Def* body);
    void get_exn_closures(const Def* def, DefSet& visited);

    /// Substitutes closure literals of tagged exception Lam%s by throw closures; does not descend into mutables.
    const Def* subst_exn_closures(const Def* def);

    // clang-format off
    LamMap<std::pair<int, const Def*>> lam2tag_;
    DefMap<Lam*> dom2throw_;
    DefMap<Lam*> lam2lpad_;
    LamSet ignore_;
    // clang-format on

    const Def* cur_rbuf_ = nullptr;
    const Def* cur_jbuf_ = nullptr;
};

} // namespace mim::plug::clos::phase
