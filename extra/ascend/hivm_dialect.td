#ifndef MIM_ASCEND_HIVM_DIALECT_TD
#define MIM_ASCEND_HIVM_DIALECT_TD

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpBase.td"

def HIVM_Dialect : Dialect {
  let name = "hivm";
  let cppNamespace = "::mlir::hivm";
  let useDefaultAttributePrinterParser = 1;
  let summary = "The subset of AscendNPU-IR's HIVM dialect that this plugin emits.";
}

// The case values mirror AscendNPU-IR's HIVM_AddressSpaceEnum.
def HIVM_GM : I32EnumAttrCase<"GM", 1, "gm">;
def HIVM_UB : I32EnumAttrCase<"UB", 6, "ub">;

def HIVM_AddressSpaceEnum : I32EnumAttr<"AddressSpace", "HIVM address space", [HIVM_GM, HIVM_UB]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::mlir::hivm";
}

def HIVM_AddressSpaceAttr : AttrDef<HIVM_Dialect, "AddressSpace"> {
  let mnemonic = "address_space";
  let parameters = (ins EnumParameter<HIVM_AddressSpaceEnum>:$address_space);
  let assemblyFormat = "`<` params `>`";
}

class HIVM_MemRef<string space, string description>
    : Type<And<[MemRefOf<[F32]>.predicate,
                CPred<"::mlir::hivm::in_address_space($_self, ::mlir::hivm::AddressSpace::" # space # ")">]>,
           "f32 memref in " # description, "::mlir::MemRefType">;

def HIVM_GMMemRef : HIVM_MemRef<"GM", "global memory">;
def HIVM_UBMemRef : HIVM_MemRef<"UB", "the unified buffer">;

class HIVM_Op<string mnemonic, list<Trait> traits = []>
    : Op<HIVM_Dialect, "hir." # mnemonic, !listconcat([SameOperandsShape, SameOperandsElementType], traits)>;

class HIVM_DmaOp<string mnemonic, Type src, Type dst> : HIVM_Op<mnemonic> {
  let arguments = (ins src:$src, dst:$dst);
  let assemblyFormat = [{
    `ins` `(` $src `:` type($src) `)` `outs` `(` $dst `:` type($dst) `)` attr-dict
  }];
}

def HIVM_LoadOp : HIVM_DmaOp<"load", HIVM_GMMemRef, HIVM_UBMemRef> {
  let summary = "Copies a global-memory buffer into the unified buffer.";
}

def HIVM_StoreOp : HIVM_DmaOp<"store", HIVM_UBMemRef, HIVM_GMMemRef> {
  let summary = "Copies a unified-buffer buffer back into global memory.";
}

class HIVM_VecBinaryOp<string mnemonic> : HIVM_Op<mnemonic> {
  let arguments = (ins HIVM_UBMemRef:$lhs, HIVM_UBMemRef:$rhs, HIVM_UBMemRef:$dst);
  let assemblyFormat = [{
    attr-dict `ins` `(` $lhs `,` $rhs `:` type($lhs) `,` type($rhs) `)`
    `outs` `(` $dst `:` type($dst) `)`
  }];
}

def HIVM_VAddOp : HIVM_VecBinaryOp<"vadd"> {
  let summary = "Elementwise addition on the vector pipeline.";
}

def HIVM_VSubOp : HIVM_VecBinaryOp<"vsub"> {
  let summary = "Elementwise subtraction on the vector pipeline.";
}

def HIVM_VMulOp : HIVM_VecBinaryOp<"vmul"> {
  let summary = "Elementwise multiplication on the vector pipeline.";
}

def HIVM_VDivOp : HIVM_VecBinaryOp<"vdiv"> {
  let summary = "Elementwise division on the vector pipeline.";
}

#endif // MIM_ASCEND_HIVM_DIALECT_TD
