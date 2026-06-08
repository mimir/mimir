#include <cstdint>

#include <optional>
#include <string>
#include <vector>

#include "mim/plug/spirv/autogen.h"

#include "fe/assert.h"

// Enums
namespace mim::plug::spirv {
using Word = int32_t;

using namespace std::string_literals;

std::vector<Word> string_to_words(std::string_view string);
template<typename Iter>
std::string words_to_string(Iter& begin, Iter end) {
    std::string out;
    Word mask = (1 << 8) - 1;
    for (; begin != end; begin++) {
        Word word = *begin;
        for (int index = 0; index < 4; index++)
            if (auto c = (word >> (index * 8)) & mask)
                out.push_back(static_cast<char>(c));
            else {
                // Advance past the null-terminator word
                ++begin;
                return out;
            }
    }
    return out;
}

std::vector<Word> float_to_words(uint64_t bits, int width);
double words_to_float(const std::vector<Word>& words, int width);

std::vector<Word> int_to_words(uint64_t value, int width);
int64_t words_to_int(const std::vector<Word>& words, int width);

// Helper for building operand vectors - supports Words and vectors of Words
class Operands : public std::vector<Word> {
public:
    Operands() = default;
    Operands(std::initializer_list<Word> words)
        : std::vector<Word>(words) {}

    Operands& operator<<(Word w) {
        push_back(w);
        return *this;
    }
    Operands& operator<<(const std::vector<Word>& ws) {
        insert(end(), ws.begin(), ws.end());
        return *this;
    }
};

namespace capability {
enum Capability {
    Shader = 1,
    Kernel = 6,
};

std::string name(int capability);
} // namespace capability

namespace addressing_model {
enum AddressingModel {
    Logical    = 0,
    Physical32 = 1,
    Physical64 = 2,
};

std::string name(int model);
} // namespace addressing_model

namespace memory_model {
enum MemoryModel {
    Simple  = 0,
    GLSL450 = 1,
    OpenCL  = 2,
    Vulkan  = 3,
};

std::string name(int model);
} // namespace memory_model

namespace execution_model {
enum ExecutionModel {
    Vertex    = 0,
    Fragment  = 4,
    GLCompute = 5,
};

std::string name(int model);
} // namespace execution_model

namespace execution_mode {
enum ExecutionMode {
    Invocations           = 0,
    SpacingEqual          = 1,
    SpacingFractionalEven = 2,
    SpacingFractionalOdd  = 3,
    VertexOrderCw         = 4,
    VertexOrderCcw        = 5,
    PixelCenterInteger    = 6,
    OriginUpperLeft       = 7,
    OriginLowerLeft       = 8,
    EarlyFragmentTests    = 9,
};

std::string name(int mode);
} // namespace execution_mode

namespace function_control {
enum FunctionControl {
    None       = 0,
    Inline     = 1,
    DontInline = 2,
    Pure       = 4,
    Const      = 8,
};

std::string name(int control);
} // namespace function_control

namespace selection_control {
enum SelectionControl {
    None        = 0,
    Flatten     = 1,
    DontFlatten = 2,
};

std::string name(int control);
} // namespace selection_control

namespace loop_control {
enum LoopControl {
    None       = 0,
    Unroll     = 1,
    DontUnroll = 2,
};

std::string name(int control);
} // namespace loop_control

namespace storage_class {
enum StorageClass {
    Input    = 1,
    Uniform  = 2,
    Output   = 3,
    Private  = 6,
    Function = 7,
};

std::string name(int storage_class);
StorageClass from_mim(::mim::plug::spirv::storage s);
bool is_local(StorageClass s);
} // namespace storage_class

namespace decoration {
enum Decoration {
    Block    = 2,
    BuiltIn  = 11,
    Location = 30,
};

std::string name(int decoration);
} // namespace decoration

namespace spv_builtin {
enum BuiltIn {
    Position      = 0,
    PointSize     = 1,
    VertexId      = 5,
    InstanceId    = 6,
    VertexIndex   = 42,
    InstanceIndex = 43,
    FragCoord     = 15,
};

std::string name(int builtin);
BuiltIn from_mim(::mim::plug::spirv::builtin b);
} // namespace spv_builtin

namespace ext_inst {
enum ExtInstSet {
    GLSLstd450 = 1,
};

std::string name(int set);
} // namespace ext_inst

enum class OpKind {
    Undefined,
    Capability,
    Extension,
    ExtInstImport,
    MemoryModel,
    EntryPoint,
    ExecutionMode,
    Decorate,
    Decl,
    Function,
    FunctionParameter,
    FunctionEnd,
    FunctionCall,
    FunDef,
    Type,
    TypeVoid,
    TypeFunction,
    TypeInt,
    TypeFloat,
    TypeVector,
    TypeArray,
    TypeStruct,
    TypePointer,
    Unreachable,
    Variable,
    AccessChain,
    Load,
    Store,
    Bitcast,
    UConvert,
    SConvert,
    Constant,
    ConstantComposite,
    CompositeConstruct,
    CompositeExtract,
    Label,
    Phi,
    SelectionMerge,
    LoopMerge,
    Branch,
    BranchConditional,
    Switch,
    Return,
    ReturnValue,
    // Int arithmetic
    IAdd,
    ISub,
    IMul,
    // Int comparisons
    IEqual,
    INotEqual,
    UGreaterThan,
    UGreaterThanEqual,
    ULessThan,
    ULessThanEqual,
    // Float comparisons
    FOrdEqual,
    FOrdNotEqual,
    FOrdLessThan,
    FOrdGreaterThan,
    FOrdLessThanEqual,
    FOrdGreaterThanEqual,
    FUnordEqual,
    FUnordNotEqual,
    FUnordLessThan,
    FUnordGreaterThan,
    FUnordLessThanEqual,
    FUnordGreaterThanEqual,
    // Bool type and constants
    TypeBool,
    ConstantTrue,
    ConstantFalse,
};
} // namespace mim::plug::spirv

// Op
namespace mim::plug::spirv {
struct Op {
    OpKind kind;
    std::vector<Word> operands;
    std::optional<Word> result;
    std::optional<Word> result_type;

    // Used to pass additional information to pretty printing
    std::vector<Word> hints;

    // Constructors allowing optional fields from the end
    Op()
        : kind(OpKind::Undefined) {}
    Op(OpKind kind)
        : kind(kind) {}
    Op(OpKind kind, std::vector<Word> operands)
        : kind(kind)
        , operands(std::move(operands)) {}
    Op(OpKind kind, std::vector<Word> operands, std::optional<Word> result)
        : kind(kind)
        , operands(std::move(operands))
        , result(result) {}
    Op(OpKind kind, std::vector<Word> operands, std::optional<Word> result, std::optional<Word> result_type)
        : kind(kind)
        , operands(std::move(operands))
        , result(result)
        , result_type(result_type) {}
    Op(OpKind kind,
       std::vector<Word> operands,
       std::optional<Word> result,
       std::optional<Word> result_type,
       std::vector<Word> hints)
        : kind(kind)
        , operands(std::move(operands))
        , result(result)
        , result_type(result_type)
        , hints(std::move(hints)) {}

    const char* name() const {
        switch (this->kind) {
            case OpKind::Undefined: return "Undefined";
            case OpKind::Capability: return "OpCapability";
            case OpKind::Extension: return "OpExtension";
            case OpKind::ExtInstImport: return "OpExtInstImport";
            case OpKind::MemoryModel: return "OpMemoryModel";
            case OpKind::EntryPoint: return "OpEntryPoint";
            case OpKind::ExecutionMode: return "OpExecutionMode";
            case OpKind::Decorate: return "OpDecorate";
            case OpKind::Decl: return "OpDecl";
            case OpKind::Function: return "OpFunction";
            case OpKind::FunctionParameter: return "OpFunctionParameter";
            case OpKind::FunctionEnd: return "OpFunctionEnd";
            case OpKind::FunctionCall: return "OpFunctionCall";
            case OpKind::Type: return "OpType";
            case OpKind::TypeVoid: return "OpTypeVoid";
            case OpKind::TypeFunction: return "OpTypeFunction";
            case OpKind::TypeInt: return "OpTypeInt";
            case OpKind::TypeFloat: return "OpTypeFloat";
            case OpKind::TypeVector: return "OpTypeVector";
            case OpKind::TypeArray: return "OpTypeArray";
            case OpKind::TypeStruct: return "OpTypeStruct";
            case OpKind::TypePointer: return "OpTypePointer";
            case OpKind::Unreachable: return "OpUnreachable";
            case OpKind::Variable: return "OpVariable";
            case OpKind::AccessChain: return "OpAccessChain";
            case OpKind::Load: return "OpLoad";
            case OpKind::Store: return "OpStore";
            case OpKind::Bitcast: return "OpBitcast";
            case OpKind::UConvert: return "OpUConvert";
            case OpKind::SConvert: return "OpSConvert";
            case OpKind::Constant: return "OpConstant";
            case OpKind::ConstantComposite: return "OpConstantComposite";
            case OpKind::CompositeConstruct: return "OpCompositeConstruct";
            case OpKind::CompositeExtract: return "OpCompositeExtract";
            case OpKind::Label: return "OpLabel";
            case OpKind::Phi: return "OpPhi";
            case OpKind::SelectionMerge: return "OpSelectionMerge";
            case OpKind::LoopMerge: return "OpLoopMerge";
            case OpKind::Branch: return "OpBranch";
            case OpKind::BranchConditional: return "OpBranchConditional";
            case OpKind::Switch: return "OpSwitch";
            case OpKind::Return: return "OpReturn";
            case OpKind::ReturnValue: return "OpReturnValue";
            case OpKind::IAdd: return "OpIAdd";
            case OpKind::ISub: return "OpISub";
            case OpKind::IMul: return "OpIMul";
            case OpKind::IEqual: return "OpIEqual";
            case OpKind::INotEqual: return "OpINotEqual";
            case OpKind::UGreaterThan: return "OpUGreaterThan";
            case OpKind::UGreaterThanEqual: return "OpUGreaterThanEqual";
            case OpKind::ULessThan: return "OpULessThan";
            case OpKind::ULessThanEqual: return "OpULessThanEqual";
            case OpKind::FOrdEqual: return "OpFOrdEqual";
            case OpKind::FOrdNotEqual: return "OpFOrdNotEqual";
            case OpKind::FOrdLessThan: return "OpFOrdLessThan";
            case OpKind::FOrdGreaterThan: return "OpFOrdGreaterThan";
            case OpKind::FOrdLessThanEqual: return "OpFOrdLessThanEqual";
            case OpKind::FOrdGreaterThanEqual: return "OpFOrdGreaterThanEqual";
            case OpKind::FUnordEqual: return "OpFUnordEqual";
            case OpKind::FUnordNotEqual: return "OpFUnordNotEqual";
            case OpKind::FUnordLessThan: return "OpFUnordLessThan";
            case OpKind::FUnordGreaterThan: return "OpFUnordGreaterThan";
            case OpKind::FUnordLessThanEqual: return "OpFUnordLessThanEqual";
            case OpKind::FUnordGreaterThanEqual: return "OpFUnordGreaterThanEqual";
            case OpKind::TypeBool: return "OpTypeBool";
            case OpKind::ConstantTrue: return "OpConstantTrue";
            case OpKind::ConstantFalse: return "OpConstantFalse";
            default: fe::unreachable();
        }
    }

    Word magic() const {
        switch (this->kind) {
            case OpKind::Capability: return 17;
            case OpKind::Extension: return 10;
            case OpKind::ExtInstImport: return 11;
            case OpKind::MemoryModel: return 14;
            case OpKind::EntryPoint: return 15;
            case OpKind::ExecutionMode: return 16;
            case OpKind::Decorate: return 71;
            case OpKind::TypeVoid: return 19;
            case OpKind::TypeFunction: return 33;
            case OpKind::TypeInt: return 21;
            case OpKind::TypeFloat: return 22;
            case OpKind::TypeVector: return 23;
            case OpKind::TypeArray: return 28;
            case OpKind::TypeStruct: return 30;
            case OpKind::TypePointer: return 32;
            case OpKind::Unreachable: return 255;
            case OpKind::Variable: return 59;
            case OpKind::AccessChain: return 65;
            case OpKind::Load: return 61;
            case OpKind::Store: return 62;
            case OpKind::Bitcast: return 124;
            case OpKind::UConvert: return 113;
            case OpKind::SConvert: return 114;
            case OpKind::Constant: return 43;
            case OpKind::ConstantComposite: return 44;
            case OpKind::CompositeConstruct: return 80;
            case OpKind::Function: return 54;
            case OpKind::FunctionParameter: return 55;
            case OpKind::FunctionEnd: return 56;
            case OpKind::FunctionCall: return 57;
            case OpKind::Label: return 248;
            case OpKind::Phi: return 245;
            case OpKind::SelectionMerge: return 247;
            case OpKind::LoopMerge: return 246;
            case OpKind::Branch: return 249;
            case OpKind::BranchConditional: return 250;
            case OpKind::Switch: return 251;
            case OpKind::Return: return 253;
            case OpKind::ReturnValue: return 254;
            case OpKind::IAdd: return 128;
            case OpKind::ISub: return 130;
            case OpKind::IMul: return 132;
            case OpKind::IEqual: return 170;
            case OpKind::INotEqual: return 171;
            case OpKind::UGreaterThan: return 172;
            case OpKind::UGreaterThanEqual: return 174;
            case OpKind::ULessThan: return 176;
            case OpKind::ULessThanEqual: return 178;
            case OpKind::FOrdEqual: return 180;
            case OpKind::FOrdNotEqual: return 182;
            case OpKind::FOrdLessThan: return 184;
            case OpKind::FOrdGreaterThan: return 186;
            case OpKind::FOrdLessThanEqual: return 188;
            case OpKind::FOrdGreaterThanEqual: return 190;
            case OpKind::FUnordEqual: return 181;
            case OpKind::FUnordNotEqual: return 183;
            case OpKind::FUnordLessThan: return 185;
            case OpKind::FUnordGreaterThan: return 187;
            case OpKind::FUnordLessThanEqual: return 189;
            case OpKind::FUnordGreaterThanEqual: return 191;
            case OpKind::TypeBool: return 20;
            case OpKind::ConstantTrue: return 41;
            case OpKind::ConstantFalse: return 42;
            default: fe::unreachable();
        }
    }
};
} // namespace mim::plug::spirv
