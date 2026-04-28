#pragma once

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
        if (auto i = globals_.find(def); i != globals_.end()) return i->second;
        if (auto i = locals_.find(def); i != locals_.end()) return i->second;

        auto place = scheduler_.smart(curr_lam_, def);
        auto& bb   = lam2bb_[place->mut()->template as<Lam>()];
        return emit_term_into(def, bb);
    }

    /// Returns an optional name for an identifier to make
    /// assembly more readable.
    std::string id_name(Word id) { return module_.id_name(id); }

private:
    /// Emit a term into a given basic block
    Word emit_term_into(const Def* def, BB& bb);
    void link_phi(Lam* lam, Lam* callee, const Def* arg);

    BB& bb(Lam* lam) {
        if (!lam2bb_.contains(lam)) error("Called basic block not in function: {} not in {}", lam, curr_lam_);
        return lam2bb_[lam];
    }

    Word next_id() { return next_id_++; }
    Word id(const Def* def) {
        if (auto i = globals_.find(def); i != globals_.end()) return i->second;
        if (auto i = locals_.find(def); i != locals_.end()) return i->second;

        error("Invalid access to id of Def not previously emitted: {}", def);
    }

    /// Takes ownership of the current module, resetting it to a fresh one.
    Module take_module() { return std::exchange(module_, Module{}); }

    Module module_;

    Word next_id_{0};
    Word glsl_ext_inst_id_{0};

    Scheduler scheduler_;
    Lam* curr_lam_ = nullptr;
    LamMap<BB> lam2bb_;

    DefMap<Word> locals_;
    DefMap<Word> globals_;

    DefMap<Word> types_;
    std::optional<Word> bool_type_id_{}; // Shared Bool type (OpTypeBool)
    std::optional<Word> i32_type_id_{};  // Shared 32-bit signed integer type for all Idx
};

void emit_asm(World&, std::ostream&);

void emit_bin(World&, std::ostream&);

} // namespace mim::plug::spirv
