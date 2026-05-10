#include "mim/plug/spirv/be/serialize.h"

#include "mim/plug/spirv/be/emit.h"
namespace mim::plug::spirv {

void Serializer::assembly(std::ostream& out) {
    out << "; SPIR-V\n"
        << "; Version: 1.0\n"
        << "; Generator: MimIR; 0\n"
        << "; Bound: " << module_.id_bound << "\n"
        << "; Schema: " << 0 << "\n";

    assembly(module_.capabilities, out);
    assembly(module_.extensions, out);
    assembly(module_.extInstImports, out);
    assembly(module_.memoryModel, out);
    assembly(module_.entryPoints, out);
    assembly(module_.executionModes, out);
    assembly(module_.debug, out);
    assembly(module_.annotations, out);
    assembly(module_.declarations, out);
    assembly(module_.funDeclarations, out);
    assembly(module_.funDefinitions, out);
}

void Serializer::binary(std::ostream& out) {
    // TODO
}

void Serializer::assembly(Op& op, std::ostream& out) {
    // result
    if (op.result.has_value()) {
        auto result = id_name(op.result.value());
        for (int i = result.length() + 4; i < indent(); i++)
            out << " ";
        out << "%" << result << " = ";
    } else {
        for (int i = 0; i < indent(); i++)
            out << " ";
    }

    // op
    out << op.name();

    // result type (if present)
    if (op.result_type.has_value()) out << " %" << id_name(op.result_type.value());

    switch (op.kind) {
        case OpKind::Capability: out << " " << capability::name(op.operands[0]); break;
        case OpKind::ExtInstImport: out << " " << ext_inst::name(op.operands[0]); break;
        case OpKind::MemoryModel:
            out << " " << addressing_model::name(op.operands[0]);
            out << " " << memory_model::name(op.operands[1]);
            break;
        case OpKind::EntryPoint: {
            auto ops = op.operands.cbegin();
            out << " " << execution_model::name(*ops++);
            out << " %" << id_name(*ops++);
            // TODO: investigate what is going slightly wrong here
            auto name = words_to_string(ops, op.operands.cend());
            out << " \"" << name << "\"";
            for (; ops != op.operands.cend(); ops++)
                out << " %" << id_name(*ops);
            break;
        }
        case OpKind::ExecutionMode:
            out << " %" << id_name(op.operands[0]);
            out << " " << execution_mode::name(op.operands[1]);
            // Some execution modes have additional literal operands
            for (size_t i = 2; i < op.operands.size(); ++i)
                out << " " << op.operands[i];
            break;
        case OpKind::Function:
            out << " " << function_control::name(op.operands[0]);
            out << " %" << id_name(op.operands[1]);
            break;
        case OpKind::Decorate:
            out << " %" << id_name(op.operands[0]);
            out << " " << decoration::name(op.operands[1]);
            if (op.operands[1] == decoration::BuiltIn && op.operands.size() > 2) {
                out << " " << spv_builtin::name(op.operands[2]);
                for (size_t i = 3; i < op.operands.size(); ++i)
                    out << " " << op.operands[i];
            } else {
                for (size_t i = 2; i < op.operands.size(); ++i)
                    out << " " << op.operands[i];
            }
            break;
        case OpKind::TypePointer:
            out << " " << storage_class::name(op.operands[0]);
            out << " %" << id_name(op.operands[1]);
            break;
        case OpKind::Variable:
            out << " " << storage_class::name(op.operands[0]);
            if (op.operands.size() > 1) // optional initializer
                out << " %" << id_name(op.operands[1]);
            break;
        case OpKind::TypeInt:
            out << " " << op.operands[0]; // width
            out << " " << op.operands[1]; // signedness: 0=unsigned, 1=signed
            break;
        case OpKind::TypeFloat:
            out << " " << op.operands[0]; // width
            break;
        case OpKind::Constant:
            // hints[0] = type (0=int, 1=float), hints[1] = width
            if (op.hints.size() >= 2 && op.hints[0] == 1) {
                out << " " << words_to_float(op.operands, op.hints[1]);
            } else if (op.hints.size() >= 2) {
                out << " " << words_to_int(op.operands, op.hints[1]);
            } else {
                // Fallback for constants without hints
                out << " " << op.operands[0];
            }
            break;
        case OpKind::TypeVector:
            out << " %" << id_name(op.operands[0]); // component type
            out << " " << op.operands[1];           // component count
            break;
        case OpKind::TypeArray:
            out << " %" << id_name(op.operands[0]); // element type
            out << " %" << id_name(op.operands[1]); // length (constant)
            break;
        case OpKind::CompositeExtract:
            out << " %" << id_name(op.operands[0]); // composite
            out << " " << op.operands[1];           // index
            break;
        case OpKind::TypeStruct:
        case OpKind::TypeFunction:
        case OpKind::ConstantComposite:
        case OpKind::CompositeConstruct:
            // All member/parameter/constituent types/values
            for (Word operand : op.operands)
                out << " %" << id_name(operand);
            break;
        case OpKind::FunctionParameter:
        case OpKind::FunctionEnd:
        case OpKind::TypeVoid:
            // No operands
            break;
        default:
            for (Word operand : op.operands)
                out << " %" << id_name(operand);
    }

    out << "\n";
}

int Serializer::max_name_length() {
    int max = 0;
    for (auto name : module_.id_names) {
        int length = name.second.length();
        if (length > max) max = length;
    }
    return max;
}

void emit_asm(World& world, std::ostream& out) {
    Emitter emitter{world};
    auto module = emitter.emit();
    Serializer serializer{module};
    serializer.assembly(out);
}

void emit_bin(World& world, std::ostream& out) {
    Emitter emitter{world};
    auto module = emitter.emit();
    Serializer serializer{module};
    serializer.binary(out);
}

} // namespace mim::plug::spirv
