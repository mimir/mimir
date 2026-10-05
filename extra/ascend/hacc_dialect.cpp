#include "hacc_dialect.h"

#include <llvm/ADT/TypeSwitch.h>

#include "hacc_dialect.cpp.inc"
#include "hacc_enums.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "hacc_attrs.cpp.inc"

void mlir::hacc::HACCDialect::initialize() {
    addAttributes<
#define GET_ATTRDEF_LIST
#include "hacc_attrs.cpp.inc"
        >();
}
