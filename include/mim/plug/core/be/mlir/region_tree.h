#pragma once
#include <cassert>
#include <cstdint>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mim::mlir_be {

struct Printer;
struct MLIRTypeNode;

// ------------- Types -------------

struct MLIRIntType {
    uint32_t bits;
};
struct MLIRIndexType {};
struct MLIRFloatType {
    uint32_t bits;
};

struct MLIRMemrefType {
    std::vector<std::optional<int64_t>> shape;
    std::shared_ptr<MLIRTypeNode> elem;
};

struct MLIRTensorType {
    std::vector<std::optional<int64_t>> shape;
    std::shared_ptr<MLIRTypeNode> elem;
};

struct MLIRFuncType {
    std::vector<MLIRTypeNode> inputs;
    std::vector<MLIRTypeNode> results;
};

struct MLIROpaqueType {
    std::string spelling;
};

using MLIRType = std::
    variant<MLIRIntType, MLIRIndexType, MLIRFloatType, MLIRMemrefType, MLIRTensorType, MLIRFuncType, MLIROpaqueType>;

struct MLIRTypeNode {
    MLIRType type;

    template<typename T>
    MLIRTypeNode(T t)
        : type(std::move(t)) {}
};

// --------------   Attributes
//
//
struct IntAttr {
    int64_t value;
    MLIRType type;
};
struct FloatAttr {
    double value;
    MLIRType type;
};
struct IndexAttr {
    int64_t value;
};

using MLIRAttr = std::variant<IntAttr, FloatAttr, IndexAttr>;

// ---------------  Values ---------------------
struct MLIRValue {
    std::string name;
    MLIRType type;

    MLIRValue() = default;
    MLIRValue(std::string name, MLIRType type)
        : name(std::move(name))
        , type(std::move(type)) {}

    bool empty() const { return name.empty(); }
};

// -------------------- Op base classc
class MLIROp {
public:
    virtual void print(Printer& p) const = 0;
    virtual ~MLIROp()                    = default;

    const std::vector<MLIRValue>& results() const { return results_; }
    const std::vector<MLIRValue>& operands() const { return operands_; }

    // Convenience for the common single-result case
    const MLIRValue& result() const {
        assert(results_.size() == 1 && "expected single result");
        return results_.front();
    }

    bool has_results() const { return !results_.empty(); }
    bool has_operands() const { return !operands_.empty(); }

protected:
    MLIROp(std::vector<MLIRValue> results, std::vector<MLIRValue> operands)
        : results_(std::move(results))
        , operands_(std::move(operands)) {}

    std::vector<MLIRValue> results_;
    std::vector<MLIRValue> operands_;
};

// -------------- Block and Region
struct MLIRBlock {
    std::string label;
    std::vector<MLIRValue> args;
    std::vector<std::unique_ptr<MLIROp>> ops;

    template<typename T, typename... Args>
    const MLIRValue& push(Args&&... args) {
        ops.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return ops.back()->result();
    }

    template<typename T, typename... Args>
    void push_void(Args&&... args) {
        ops.push_back(std::make_unique<T>(std::forward<Args>(args)...));
    }
};

struct MLIRRegion {
    std::vector<MLIRBlock> blocks;

    MLIRBlock& entry() { return blocks.front(); }
    const MLIRBlock& entry() const { return blocks.front(); }

    MLIRBlock& add_block(std::string label = {}) { return blocks.emplace_back(MLIRBlock{std::move(label), {}, {}}); }

    MLIRRegion() { blocks.emplace_back(); }
};
// ------------  Functions -------------------

std::string print_type(const MLIRType& t);
std::string print_attr(const MLIRAttr& a);

} // namespace mim::mlir_be
