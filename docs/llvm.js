/**

Lexes the LLVM IR the ll backend emits; see `docs/code.js` for the machinery.
This is not all of LLVM — just the keywords, types, and literals such a listing actually shows.

*/

class LlvmCode extends Code {
    static WORDS = {
        keyword: new Set(["acq_rel", "acquire", "add", "addrspacecast", "align", "alloca", "and", "ashr",
                          "atomic", "atomicrmw", "bitcast", "br", "call", "callbr", "catchret", "catchswitch",
                          "ccc", "cleanupret", "cmpxchg", "coldcc", "common", "dso_local", "eq", "exact",
                          "external", "extractelement", "extractvalue", "fadd", "fast", "fastcc", "fcmp", "fdiv",
                          "fence", "fmul", "fneg", "fpext", "fptosi", "fptoui", "fptrunc", "freeze", "frem",
                          "fsub", "getelementptr", "hidden", "icmp", "inbounds", "indirectbr", "insertelement",
                          "insertvalue", "internal", "invoke", "landingpad", "load", "lshr", "monotonic", "mul",
                          "ne", "nounwind", "nsw", "nuw", "oeq", "oge", "ogt", "ole", "olt", "one", "or", "ord",
                          "phi", "private", "protected", "ptrtoint", "readnone", "readonly", "release", "resume",
                          "ret", "sdiv", "select", "seq_cst", "sext", "sge", "sgt", "shl", "shufflevector",
                          "sitofp", "sle", "slt", "srem", "store", "sub", "swiftcc", "switch", "tail", "tailcc",
                          "thread_local", "to", "trunc", "udiv", "ueq", "uge", "ugt", "uitofp", "ule", "ult",
                          "une", "unordered", "unreachable", "uno", "urem", "va_arg", "volatile", "weak", "xor",
                          "zext"]),
        decl: new Set(["alias", "asm", "attributes", "constant", "datalayout", "declare", "define", "global",
                       "ifunc", "module", "source_filename", "target", "triple", "type"]),
        type: new Set(["bfloat", "double", "float", "fp128", "half", "label", "metadata", "opaque", "ppc_fp128",
                       "ptr", "token", "void", "x86_fp80"]),
        literal: new Set(["false", "none", "null", "poison", "true", "undef", "zeroinitializer"])
    }
    static TOKEN = /(;.*)|("(?:\\.|[^"\\])*")|(@(?:"(?:\\.|[^"\\])*"|[-\w$.]+))|(%(?:"(?:\\.|[^"\\])*"|[-\w$.]+))|([!#][-\w$.]*)|((?<![^\n])[-\w$.]+:)|([A-Za-z_][\w.]*)|(-?\d[\w.]*)|(\s+|[^])/uy
    static KINDS = ["comment", "string", "global", "local", "meta", "label", "word", "number", null]
    static CLASS = {...Code.CLASS, global: "token-special", label: "token-special", meta: "token-comment"}

    static kind(kind, text) {
        if (kind === "word" && /^i\d+$/.test(text)) return "type"
        return super.kind(kind, text)
    }
}

LlvmCode.register("llvm")
