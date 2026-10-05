#ifndef MIM_ASCEND_HACC_DIALECT_TD
#define MIM_ASCEND_HACC_DIALECT_TD

include "mlir/IR/AttrTypeBase.td"
include "mlir/IR/EnumAttr.td"

def HACC_Dialect : Dialect {
  let name = "hacc";
  let cppNamespace = "::mlir::hacc";
  let useDefaultAttributePrinterParser = 1;
  let summary = "The subset of AscendNPU-IR's HACC dialect that this plugin emits.";
}

def HACC_Host : I32EnumAttrCase<"HOST", 1>;
def HACC_Device : I32EnumAttrCase<"DEVICE", 2>;

def HACC_FuncTypeEnum
    : I32EnumAttr<"HACCFuncType", "HACC function category", [HACC_Host, HACC_Device]> {
  let genSpecializedAttr = 0;
  let cppNamespace = "::mlir::hacc";
}

def HACC_FuncTypeAttr : AttrDef<HACC_Dialect, "HACCFuncType"> {
  let mnemonic = "function_kind";
  let parameters = (ins EnumParameter<HACC_FuncTypeEnum>:$function_kind);
  let assemblyFormat = "`<` params `>`";
}

#endif // MIM_ASCEND_HACC_DIALECT_TD
