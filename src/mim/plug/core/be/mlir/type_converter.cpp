#include "mim/plug/core/be/mlir/type_converter.h"

#include <mim/def.h>

#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

namespace mim::mlir_be {

MLIRType TypeConverter::convert(const Def* type) {
    // check cache first
    if (auto it = cache_.find(type); it != cache_.end()) return it->second;

    MLIRType result = [&]() -> MLIRType {
        if (type->isa<Nat>()) return MLIRIntType{64};

        if (auto size = Idx::isa(type)) {
            auto bits = Idx::size2bitwidth(size);
            return MLIRIntType{bits ? static_cast<uint32_t>(*bits) : 64u};
        }

        if (auto w = plug::math::isa_f(type)) return MLIRFloatType{static_cast<uint32_t>(*w)};

        if (auto ptr = Axm::isa<plug::mem::Ptr>(type)) {
            auto [pointee, _addr] = ptr->args<2>();
            MLIRMemrefType m;
            m.shape = {}; // scalar pointer — 0-d memref
            m.elem  = std::make_shared<MLIRTypeNode>(convert(pointee));
            return m;
        }

        if (auto arr = type->isa<Arr>()) {
            MLIRTensorType t;
            // flatten nested arrays into a single shape vector
            const Def* cur = arr;
            while (auto a = cur->isa<Arr>()) {
                if (auto n = Lit::isa(a->arity()))
                    t.shape.push_back(static_cast<int64_t>(*n));
                else
                    t.shape.push_back(std::nullopt); // dynamic
                cur = a->body();
            }
            t.elem = std::make_shared<MLIRTypeNode>(convert(cur));
            return t;
        }

        // Sigma [mem::M, T] → T  (strip mem)
        if (auto sigma = type->isa<Sigma>()) {
            if (sigma->num_ops() == 2 && Axm::isa<plug::mem::M>(sigma->op(0))) return convert(sigma->op(1));

            // general heterogeneous sigma → opaque fallback
            std::string s = "!llvm.struct<(";
            bool first    = true;
            for (auto op : sigma->ops()) {
                if (Axm::isa<plug::mem::M>(op)) continue;
                if (!first) s += ", ";
                s += print_type(convert(op));
                first = false;
            }
            s += ")>";
            return MLIROpaqueType{s};
        }

        // fallback
        return MLIROpaqueType{"!llvm.ptr"};
    }();

    cache_[type] = result;
    return result;
}

std::string TypeConverter::ret_type_str(const Pi* pi) {
    if (is_void_ret(pi)) return "";

    auto ret_pi = pi->ret_pi();
    if (!ret_pi) return "";

    // strip mem from return domain
    std::vector<std::string> types;
    auto dom = ret_pi->dom();
    if (auto sigma = dom->isa<Sigma>()) {
        for (auto op : sigma->ops()) {
            if (Axm::isa<plug::mem::M>(op)) continue;
            types.push_back(print_type(convert(op)));
        }
    } else if (!Axm::isa<plug::mem::M>(dom)) {
        types.push_back(print_type(convert(dom)));
    }

    if (types.empty()) return "";
    if (types.size() == 1) return types[0];

    std::string s = "(";
    for (auto& t : types)
        s += (&t == &types.front() ? "" : ", ") + t;
    return s + ")";
}

bool TypeConverter::is_void_ret(const Pi* pi) const {
    auto ret_pi = pi->ret_pi();
    if (!ret_pi) return true;
    auto dom = plug::mem::strip_mem_ty(ret_pi->dom());
    return dom == world_.sigma();
}

MLIRValue TypeConverter::to_index(MLIRValue v, MLIRBlock& into, std::string fresh_name) {
    if (std::holds_alternative<MLIRIndexType>(v.type)) return v;
    MLIRValue cast{fresh_name, MLIRType{MLIRIndexType{}}};
    into.ops.emplace_back(std::make_unique<IndexCastOp>(cast, v));
    return cast;
}

} // namespace mim::mlir_be
