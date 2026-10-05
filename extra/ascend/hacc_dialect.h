#pragma once

#include <mlir/IR/Attributes.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/BuiltinAttributes.h>
#include <mlir/IR/Dialect.h>
#include <mlir/IR/DialectImplementation.h>

#include "hacc_dialect.h.inc"
#include "hacc_enums.h.inc"

#define GET_ATTRDEF_CLASSES
#include "hacc_attrs.h.inc"
