#pragma once

#include "mim/plug/spirv/be/emit.h"
namespace mim::plug::spirv {

class Serializer {
public:
    Serializer(Emitter::Module module)
        : module_(module) {}

    void assembly(std::ostream&);
    void binary(std::ostream&);

private:
    int max_name_length();
    int indent() {
        if (indent_ == -1) indent_ = max_name_length() + 4;
        return indent_;
    }

    std::string id_name(Word id) { return module_.id_name(id); }

    void assembly(OpVec& ops, std::ostream& out) {
        for (Op op : ops)
            assembly(op, out);
    }
    void assembly(Op& op, std::ostream&);

    Emitter::Module module_;
    int indent_ = -1;
};

} // namespace mim::plug::spirv
