#include "hivm_dialect.h"

#include <llvm/ADT/TypeSwitch.h>

#include "hivm_dialect.cpp.inc"
#include "hivm_enums.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "hivm_attrs.cpp.inc"

#define GET_OP_CLASSES
#include "hivm_ops.cpp.inc"

namespace mlir::hivm {

bool in_address_space(Type type, AddressSpace space) {
    auto memref = dyn_cast<MemRefType>(type);
    if (!memref) return false;
    auto attr = dyn_cast_or_null<AddressSpaceAttr>(memref.getMemorySpace());
    return attr && attr.getAddressSpace() == space;
}

void HIVMDialect::initialize() {
    addAttributes<
#define GET_ATTRDEF_LIST
#include "hivm_attrs.cpp.inc"
        >();
    addOperations<
#define GET_OP_LIST
#include "hivm_ops.cpp.inc"
        >();
}

} // namespace mlir::hivm
