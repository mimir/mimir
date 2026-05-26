#include "region_tree.h"

#include <format>
#include <stdexcept>

namespace mim::mlir_be {

std::string print_type(const MLIRTypeNode& n) { return print_type(n.type); }

std::string print_type(const MLIRType& t) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, MLIRIntType>)
                return std::format("i{}", v.bits);

            else if constexpr (std::is_same_v<T, MLIRIndexType>)
                return "index";

            else if constexpr (std::is_same_v<T, MLIRFloatType>) {
                switch (v.bits) {
                    case 16: return "f16";
                    case 32: return "f32";
                    case 64: return "f64";
                    default: throw std::logic_error(std::format("unsupported float width {}", v.bits));
                }
            }

            else if constexpr (std::is_same_v<T, MLIRMemrefType>) {
                std::string s = "memref<";
                for (auto& dim : v.shape)
                    s += dim ? std::format("{}x", *dim) : "?x";
                s += print_type(*v.elem) + ">";
                return s;
            }

            else if constexpr (std::is_same_v<T, MLIRTensorType>) {
                std::string s = "tensor<";
                for (auto& dim : v.shape)
                    s += dim ? std::format("{}x", *dim) : "?x";
                s += print_type(*v.elem) + ">";
                return s;
            }

            else if constexpr (std::is_same_v<T, MLIRFuncType>) {
                std::string ins, outs;
                for (auto& n : v.inputs)
                    ins += (ins.empty() ? "" : ", ") + print_type(n);
                for (auto& n : v.results)
                    outs += (outs.empty() ? "" : ", ") + print_type(n);
                return std::format("({}) -> ({})", ins, outs);
            }

            else if constexpr (std::is_same_v<T, MLIROpaqueType>)
                return v.spelling;
        },
        t);
}
std::string print_attr(const MLIRAttr& a) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, IntAttr>)
                return std::format("{} : {}", v.value, print_type(v.type));

            else if constexpr (std::is_same_v<T, FloatAttr>)
                return std::format("{} : {}", v.value, print_type(v.type));

            else if constexpr (std::is_same_v<T, IndexAttr>)
                return std::format("{} : index", v.value);
        },
        a);
}
} // namespace mim::mlir_be
