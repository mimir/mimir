#pragma once

#include <mlir/IR/Attributes.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/Dialect.h>
#include <mlir/IR/DialectImplementation.h>
#include <mlir/IR/OpDefinition.h>
#include <mlir/IR/OpImplementation.h>

#include "hivm_dialect.h.inc"
#include "hivm_enums.h.inc"

#define GET_ATTRDEF_CLASSES
#include "hivm_attrs.h.inc"

namespace mlir::hivm {

/// Whether @p type is a memref annotated with `#hivm.address_space<@p space>`; used by the generated verifiers.
bool in_address_space(Type type, AddressSpace space);

} // namespace mlir::hivm

#define GET_OP_CLASSES
#include "hivm_ops.h.inc"
