#pragma once

#include <mim/phase/ds2cps.h>

namespace mim::plug::mach {

/// Turns every `Nat` value and every pointer into an `I64`, so that instruction selection only sees register types.
/// `Nat` literals stay `Nat` as type-level arguments and become `I64` only as values.
/// `mem.lea` becomes a `core.wrap.add` of the byte offset by the layout of `core.trait`, `mem.load`/`mem.store` become
/// `mach.load`/`mach.store`, `mem.mslot` becomes `mach.slot`, and the `Nat` operations become their `I64` counterparts.
/// `mem.malloc`/`mem.free` become calls of the C functions.
class Legalize : public DS2CPS {
public:
    using DS2CPS::DS2CPS;

private:
    /// Are the rewritten @p args no type-level `Nat`s, such as a size bound by a Pi?
    bool is_value(const Def* args);
    /// The rewritten @p def with a `Nat` literal as `I64`.
    const Def* val(const Def* def);
    /// The rewritten array index @p i at register size.
    const Def* index(const Def* i);
    /// Lifts the call @p app of the C function @p name: `Cn [mem.M 0, I64, Cn T]` for the result type `T` of @p app.
    const Def* call_c(Lam*& decl, const char* name, const App* app, Defs args);

    const Def* rewrite_imm_Nat(const Nat*) final { return new_world().type_i64(); }
    const Def* rewrite_imm_Lit(const Lit*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;

    Lam* malloc_ = nullptr;
    Lam* free_   = nullptr;
};

} // namespace mim::plug::mach
