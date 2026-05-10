#include "mim/plug/spirv/be/op.h"

#include <cstring>

#include <iostream>

namespace mim::plug::spirv {

std::vector<Word> float_to_words(uint64_t bits, int width) {
    // Input bits are always 64-bit double IEEE 754 representation
    // Convert to the target width
    double d;
    std::memcpy(&d, &bits, sizeof(d));

    switch (width) {
        case 16: {
            // Half precision: convert double to half
            // TODO: proper half-float conversion
            float f = static_cast<float>(d);
            uint32_t f_bits;
            std::memcpy(&f_bits, &f, sizeof(f_bits));
            // Simple conversion: just truncate to 16 bits (not accurate for half-float)
            return {static_cast<Word>(f_bits >> 16)};
        }
        case 32: {
            // Single precision: convert double to float
            float f = static_cast<float>(d);
            uint32_t f_bits;
            std::memcpy(&f_bits, &f, sizeof(f_bits));
            return {static_cast<Word>(f_bits)};
        }
        case 64:
            // Double precision: use bits directly
            return {static_cast<Word>(bits & 0xFFFFFFFF), static_cast<Word>((bits >> 32) & 0xFFFFFFFF)};
        default: fe::unreachable();
    }
}

double words_to_float(const std::vector<Word>& words, int width) {
    switch (width) {
        case 16: {
            // Half precision: convert to float for display
            // TODO: proper half-float conversion
            return static_cast<double>(words[0]);
        }
        case 32: {
            float f;
            uint32_t bits = static_cast<uint32_t>(words[0]);
            std::memcpy(&f, &bits, sizeof(f));
            return static_cast<double>(f);
        }
        case 64: {
            double d;
            uint64_t bits = static_cast<uint64_t>(words[0]) | (static_cast<uint64_t>(words[1]) << 32);
            std::memcpy(&d, &bits, sizeof(d));
            return d;
        }
        default: fe::unreachable();
    }
}

std::vector<Word> int_to_words(uint64_t value, int width) {
    switch (width) {
        case 8:
        case 16:
        case 32: return {static_cast<Word>(value & 0xFFFFFFFF)};
        case 64: return {static_cast<Word>(value & 0xFFFFFFFF), static_cast<Word>((value >> 32) & 0xFFFFFFFF)};
        default: fe::unreachable();
    }
}

int64_t words_to_int(const std::vector<Word>& words, int width) {
    switch (width) {
        case 8: return static_cast<int8_t>(words[0] & 0xFF);
        case 16: return static_cast<int16_t>(words[0] & 0xFFFF);
        case 32: return static_cast<int32_t>(words[0]);
        case 64: return static_cast<int64_t>(static_cast<uint64_t>(words[0]) | (static_cast<uint64_t>(words[1]) << 32));
        default: fe::unreachable();
    }
}

std::vector<Word> string_to_words(std::string_view string) {
    std::vector<Word> out{};
    int index = 0;
    Word word = 0;
    for (auto c : string) {
        word |= c << (8 * index);
        index = (index + 1) % 4;
        if (index == 0) {
            out.push_back(word);
            word = 0;
        }
    }
    // push the last partially filled word (if any) or add null terminator word
    out.push_back(word);
    return out;
}

namespace capability {
std::string name(int capability) {
    switch (capability) {
        case Shader: return "Shader"s;
        case Kernel: return "Kernel"s;
        default: std::cerr << "wtf\n"; fe::unreachable();
    }
}
} // namespace capability

namespace addressing_model {
std::string name(int model) {
    switch (model) {
        case Logical: return "Logical"s;
        case Physical32: return "Physical32"s;
        case Physical64: return "Physical64"s;
        default: fe::unreachable();
    }
}
} // namespace addressing_model

namespace memory_model {
std::string name(int model) {
    switch (model) {
        case Simple: return "Simple"s;
        case GLSL450: return "GLSL450"s;
        case OpenCL: return "OpenCL"s;
        case Vulkan: return "Vulkan"s;
        default: fe::unreachable();
    }
}
} // namespace memory_model

namespace execution_model {
std::string name(int model) {
    switch (model) {
        case Vertex: return "Vertex"s;
        case Fragment: return "Fragment"s;
        case GLCompute: return "GLCompute"s;
        default: fe::unreachable();
    }
}
} // namespace execution_model

namespace function_control {
std::string name(int control) {
    switch (control) {
        case None: return "None"s;
        case Inline: return "Inline"s;
        case DontInline: return "DontInline"s;
        case Pure: return "Pure"s;
        case Const: return "Const"s;
        default: fe::unreachable();
    }
}
} // namespace function_control

namespace execution_mode {
std::string name(int mode) {
    switch (mode) {
        case Invocations: return "Invocations"s;
        case SpacingEqual: return "SpacingEqual"s;
        case SpacingFractionalEven: return "SpacingFractionalEven"s;
        case SpacingFractionalOdd: return "SpacingFractionalOdd"s;
        case VertexOrderCw: return "VertexOrderCw"s;
        case VertexOrderCcw: return "VertexOrderCcw"s;
        case PixelCenterInteger: return "PixelCenterInteger"s;
        case OriginUpperLeft: return "OriginUpperLeft"s;
        case OriginLowerLeft: return "OriginLowerLeft"s;
        case EarlyFragmentTests: return "EarlyFragmentTests"s;
        default: fe::unreachable();
    }
}
} // namespace execution_mode

namespace storage_class {
std::string name(int storage_class) {
    switch (storage_class) {
        case Input: return "Input"s;
        case Uniform: return "Uniform"s;
        case Output: return "Output"s;
        case Private: return "Private"s;
        case Function: return "Function"s;
        default: fe::unreachable();
    }
}

StorageClass from_mim(spirv::storage s) {
    switch (s) {
        case spirv::storage::INPUT: return Input;
        case spirv::storage::UNIFORM: return Uniform;
        case spirv::storage::OUTPUT: return Output;
        case spirv::storage::PRIVATE: return Private;
        case spirv::storage::FUNCTION: return Function;
        default: fe::unreachable();
    }
}
} // namespace storage_class

namespace decoration {
std::string name(int decoration) {
    switch (decoration) {
        case Block: return "Block"s;
        case BuiltIn: return "BuiltIn"s;
        case Location: return "Location"s;
        default: fe::unreachable();
    }
}
} // namespace decoration

namespace spv_builtin {
std::string name(int builtin) {
    switch (builtin) {
        case Position: return "Position"s;
        case PointSize: return "PointSize"s;
        case VertexId: return "VertexId"s;
        case InstanceId: return "InstanceId"s;
        case VertexIndex: return "VertexIndex"s;
        case InstanceIndex: return "InstanceIndex"s;
        case FragCoord: return "FragCoord"s;
        default: return std::to_string(builtin);
    }
}
} // namespace spv_builtin

namespace ext_inst {
std::string name(int set) {
    switch (set) {
        case GLSLstd450: return "\"GLSL.std.450\""s;
        default: fe::unreachable();
    }
}
} // namespace ext_inst
} // namespace mim::plug::spirv
