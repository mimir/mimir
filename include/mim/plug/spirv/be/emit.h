#pragma once

#include <cctype>

#include <ostream>

#include "mim/phase.h"
#include "mim/schedule.h"
#include "mim/world.h"

#include "mim/plug/spirv/be/op.h"

namespace mim::plug::spirv {

using OpVec = Vector<Op>;

/// A SPIR-V basic block
struct BB {
    Op label;
    DefMap<std::vector<Word>> phis;
    OpVec ops;
    OpVec tail;
    std::optional<Op> merge;
    Op end;
};

/// SPIR-V emitter for MimIR. Translates a given MimIR world into a SPIR-V module
/// where each external function gets mapped to an entry point.
class Emitter : NestPhase<Lam> {
    using Super = NestPhase<Lam>;

public:
    const Def* strip(const Def*);
    const Def* strip_rec(const Def*);

    struct Module {
        /// Returns an optional name for an identifier to make
        /// assembly more readable.
        std::string id_name(Word id) {
            auto it = id_names.find(id);
            if (it != id_names.end()) return it->second;
            return std::to_string(id);
        }

        OpVec capabilities;
        OpVec extensions;
        OpVec extInstImports;
        Op memoryModel;
        OpVec entryPoints;
        OpVec executionModes;
        OpVec debug;
        OpVec annotations;
        OpVec declarations;
        OpVec funDeclarations;
        OpVec funDefinitions;

        absl::flat_hash_map<Word, std::string> id_names;
        Word id_bound = -1;
    };

    Emitter(World& world);

    /// Emit the entire world and return the resulting SPIR-V module.
    Module emit() {
        module_.capabilities.push_back(Op{OpKind::Capability, {capability::Shader}});
        module_.memoryModel = Op{
            OpKind::MemoryModel,
            {addressing_model::Logical, memory_model::GLSL450}
        };
        Super::start();
        module_.id_bound = next_id();
        return take_module();
    }

    void visit(const Nest& nest) override;

    /// Emit a SPIR-V top level function
    Word emit_function(Lam* fun);

    /// Emit a SPIR-V basic block
    Word emit_bb(Lam* lam, BB& bb);

    /// Convert a MimIR type into a SPIR-V type
    Word emit_type(const Def* type);

    Word emit_entry(std::string name, const Def* ptr);
    Word emit_interface(std::string name, const Def* ptr);
    void emit_decoration(Word var_id, const Def* decoration_);

    /// Translate a MimIR expression to SPIR-V
    Word emit_term(const Def* def) {
        auto stripped = strip(def);

        if (auto i = globals_.find(stripped); i != globals_.end()) return i->second;
        if (auto i = locals_.find(stripped); i != locals_.end()) return i->second;

        auto place = scheduler_.smart(curr_function_, def);
        auto& bb   = lam2bb_[place->mut()->template as<Lam>()];
        return emit_term_into(stripped, bb);
    }

    /// Returns an optional name for an identifier to make
    /// assembly more readable.
    std::string id_name(Word id) { return module_.id_name(id); }

    /// Registers a human-readable name for `id`, sanitized to a valid SPIR-V
    /// assembly identifier.
    /// SPIR-V only permits `[a-zA-Z0-9_]` in `%`-names, but Mim symbols may
    /// contain `%`, `.`, and others (e.g. `%core.mode.us`), so replace every
    /// disallowed character with `_`.
    void set_id_name(Word id, std::string name) {
        for (auto& c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
        module_.id_names[id] = std::move(name);
    }

private:
    /// Emit a term into a given basic block
    Word emit_term_into(const Def* def, BB& bb);
    void link_phi(Lam* from, Lam* callee, const Def* arg);
    void link_phi(Lam* from, Lam* callee, Word value_id);

    void finalize_function(Lam* fun);

    /// Recover the constructor argument tuple owning an `If`/`Switch`/`Loop` capability.
    const Def* cf_args(const Def* cf_struct);

    /// Recover the synthesized latch lam of the loop owning a `Loop` capability.
    Lam* cf_latch(const Def* cf_struct);

    /// Append a finished basic block to the function definition stream.
    void append_bb(BB& bb);

    /// Append `lam` and, recursively, its successors in SPIR-V layout order,
    /// derived purely from the scf terminators: construct regions first,
    /// exits (break/merge targets, loop latches) behind them.
    void layout_append(Lam* lam, MutSet& done);

    BB& bb(Lam* lam) {
        if (!lam2bb_.contains(lam)) error("Called basic block not in function: {} not in {}", lam, curr_function_);
        return lam2bb_[lam];
    }

    Word next_id() { return next_id_++; }

    Word bb_id(Lam* function) {
        if (auto i = locals_.find(function); i != locals_.end()) return i->second;

        locals_[function] = next_id();

        return locals_[function];
    }

    Word function_id(Lam* function) {
        if (auto i = globals_.find(function); i != globals_.end()) return i->second;

        globals_[function] = next_id();

        return globals_[function];
    }

    /// Takes ownership of the current module, resetting it to a fresh one.
    Module take_module() { return std::exchange(module_, Module{}); }

    Module module_;

    Word next_id_{0};
    Word glsl_ext_inst_id_{0};

    Scheduler scheduler_;
    Lam* curr_function_ = nullptr;
    LamMap<BB> lam2bb_;

    /// The current function's return continuation: either a plain CPS ret var
    /// or a parameter of polymorphic scf return type (`%scf.Ret`).
    /// Computed per nest in `visit`.
    const Def* ret_var_ = nullptr;

    DefMap<Word> interface_vars_;

    OpVec function_vars_{};
    DefMap<Word> locals_;
    DefMap<Word> globals_;

    /// Maps an scf scope key (a `(path, step)` tuple, see `scope_key_of`) to
    /// the argument tuple of the `if`/`switch`/`loop` constructor that
    /// introduced it, so exits can recover their target lams. Populated by a
    /// pre-pass in `visit`.
    DefMap<const Def*> cf_constructs_;

    /// Maps a loop's scope key to its synthesized latch lam: the unique
    /// back-edge block that every `%scf.continue` site branches through.
    /// Populated by the same pre-pass in `visit`.
    DefMap<Lam*> cf_latches_;

    /// Maps a loop header lam to its synthesized latch lam, so `finalize_function`
    /// can place the latch right after its header inside the loop's block range.
    LamMap<Lam*> latch_of_header_;

    DefMap<Word> types_;
    std::optional<Word> bool_type_id_{}; // Shared Bool type (OpTypeBool)
    std::optional<Word> i32_type_id_{};  // Shared 32-bit signed integer type for all Idx
};

bool is_scalar_type(const Def* type);
bool is_const(const Def* def);

/// The scope key identifying the `If`/`Switch`/`Loop` construct that owns a
/// capability value: a `(path, step)` tuple, the first two explicit
/// arguments of its type (capabilities are path-indexed post ROOT REDESIGN,
/// not token-indexed -- see scf.mim).
const Def* scope_key_of(const Def* cf_struct);

/// The same scope key, derived from a token value's type (`Token path step`)
/// instead of a capability's. Used to register `if`/`switch` constructors,
/// which take the token directly rather than a capability.
const Def* scope_key_of_token(const Def* token);

/// Matches `%scf.Ret R` (the opaque polymorphic return continuation
/// wrapper); yields the return payload type `R` on a match.
const Def* isa_ret(const Def* def);

void emit_asm(World& world, std::ostream& out);
void emit_bin(World& world, std::ostream& out);

} // namespace mim::plug::spirv
