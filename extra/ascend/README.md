# The `ascend` plugin

This plugin lowers one direct-style MimIR function to an MLIR module for AscendNPU-IR. A test picks the phase with `_compile`:

```mim
extern lam _compile (): compile.Phase = ascend.emit_hivm;
```

`_compile` is internalized by `optimize` before the phase runs, so the emitter sees only the kernel. `-X ascend:o=<file>` chooses the output (`-` is stdout). With no `-X`, the file is `<world>.ascend.mlir`.

Both phases build their module with the MLIR C++ API and run the MLIR verifier over it before printing. Neither prints MLIR text by hand.

## Files

| File | Owns |
| --- | --- |
| `ascend.mim` | The two phase axioms. Bootstrap turns them into `build/include/mim/plug/ascend/autogen.h`. |
| `ascend.cpp` | `mim_get_plugin`. Registers both phases and the `-X` arguments. No lowering. |
| `kernel.h`, `kernel.cpp` | `analyze`: the MimIR world reduced to a `Kernel`, or a diagnostic. No MLIR. |
| `emit.h`, `emit.cpp` | What the two emitters share: `EmitPhase`, the HACC-annotated entry function, verification and printing. |
| `hfusion.cpp` | `emit_hfusion`. Builds a Linalg-on-tensors module. |
| `hivm.cpp` | `emit_hivm`. Builds an HIVM module with explicit GM/UB placement. |
| `hivm_dialect.td`, `hacc_dialect.td` | TableGen definitions of the dialect subset this plugin constructs. |
| `hivm_dialect.*`, `hacc_dialect.*` | The hand-written half of those dialects: includes of the generated code plus `initialize`. |
| `lit/` | FileCheck tests. They do not need CANN. |

## How a phase is reached

```
ascend.mim axiom
    -> autogen.h enum class (emit_hfusion / emit_hivm)
    -> Phase::hook in register_hfusion / register_hivm
    -> EmitPhase::start
```

`Phase::hook<Axiom, PhaseClass>` keys the phase class on the axiom's flags. The C++ class must not be named `emit_hivm` or `emit_hfusion`: those names are the axiom enum classes, and a class with the same name makes `Phase::hook<ascend::emit_hivm, ...>` fail to compile.

## `analyze` and `Kernel`

`analyze` walks the body in post-order and returns a `Kernel`: a DAG of same-shape elementwise binaries over the parameters. Every value has an id. The first `num_params` ids are the parameters; the rest index `nodes`. A `Def` is entered into the id map on first use, so a shared subexpression becomes one node with two users.

A kernel `add(a, b)` has two parameters, one `add` node, and `result` pointing at it. `((a + b) - a) * b / a` has two parameters and four nodes.

### What `analyze` accepts

- One set, direct-style external `Lam` (not a continuation).
- A non-empty parameter list. Every parameter and the result have the same static F32 shape. Rank is the nesting of `Arr`, so `«4; «4; F32»»` is `4x4xf32`.
- A body that is a DAG of `tensor.binary` applied to `math.arith` `add`, `sub`, `mul`, or `div`, at mode `0`, with an F32 scalar type.

`tensor.binary` has four curried pieces: element types, the scalar function, the shape, and the operand tuple. `match_arith` looks only at the scalar function, which is `uncurry_args<4>()` index 1; the two tensor operands are index 3, projected into a pair.

Everything else is rejected with an `fe::throwf` diagnostic that names the offending `Def`. That happens before any MLIR object exists, which matters: the MLIR libraries are compiled without exception unwind tables, so an exception crossing an MLIR frame aborts. `EmitPhase::start` therefore runs `analyze`, then `check`, and only then `build`, which reports verifier output through `Built::error` instead of throwing.

## `emit_hfusion` (`hfusion.cpp`)

One `func.func` over tensors that returns a tensor. Each node becomes a `tensor.empty` plus a `linalg.elemwise_binary` with the matching `#linalg.binary_fn`. No memory is allocated and neither GM nor UB is mentioned: this is the comparison path, and the vendor compiler is expected to fuse and schedule it.

## `emit_hivm` (`hivm.cpp`)

The HIVM kernel is void. Every tensor argument is a global-memory memref, and the MimIR result becomes one extra memref argument that the kernel writes. Everything the kernel computes lives in the unified buffer.

`build` emits, in order, one `memref.alloc` plus `hivm.hir.load` per parameter the kernel actually reads, then one `memref.alloc` plus `hivm.hir.v*` per node, then one `hivm.hir.store` into the last argument. An unread parameter costs nothing.

`check` rejects a kernel whose unified-buffer footprint, `num_elems * 4 * buffers`, exceeds `-X ascend:ub`. The default, `ascend950_ub_bytes`, is `UB_SIZE` from the Ascend950PR_9579 target spec (`2031616`).

## The local dialects

CANN ships `bishengir-compile` as a static binary and does not ship the HIVM C++ headers, so `hivm_dialect.td` and `hacc_dialect.td` define the slice of those dialects that this plugin constructs. They are print-compatible with AscendNPU-IR, not a copy of it: the names, mnemonics, enum case values and assembly formats mirror `bishengir/include/bishengir/Dialect/{HIVM,HACC}/IR/*.td`, but the vendor's optional operands and attributes (padding, atomics, eviction policy, core type, tensor results) are left out, because this plugin never sets them and a definition that accepted them would not be able to honour them.

What the dialects give the emitters:

```text
#hivm.address_space<gm>     AddressSpace::GM
#hivm.address_space<ub>     AddressSpace::UB
hivm.hir.load  ins(...) outs(...)
hivm.hir.store ins(...) outs(...)
hivm.hir.vadd  ins(lhs, rhs : ...) outs(dst : ...)
#hacc.function_kind<DEVICE>
```

`hacc.entry` is a plain unit attribute, so there is nothing to define for it; AscendNPU-IR does not model it as a dialect attribute either.

Operand constraints are declarative. `HIVM_GMMemRef` and `HIVM_UBMemRef` are f32 memrefs in a fixed address space, and every op carries `SameOperandsShape` and `SameOperandsElementType`, so `mlir-tblgen` generates the verifier that used to be written by hand. A failure there is a bug in `build`: `analyze` should already have rejected a bad kernel.

### Adding an operation

1. Add a `def` to `hivm_dialect.td`. Another two-input, one-output vector instruction is one line, `def HIVM_VFooOp : HIVM_VecBinaryOp<"vfoo">`; a different operand shape needs its own class with an `arguments` list and an `assemblyFormat`.
2. Teach `match_arith` (`kernel.cpp`) to recognise the MimIR axiom and `create_binary` (`hivm.cpp`) to create the op.
3. Add a `lit` test for the new line, and a rejection test if the axiom is only partly supported.

Registration is automatic: `HIVMDialect::initialize` adds whatever `GET_OP_LIST` expands to.

A unary operation does not fit `Kernel::Node`, which has two operands. Give it its own node kind, or make the operand list variable.

### Adding an element type

`shape_of` in `kernel.cpp` rejects anything that is not F32, and the `HIVM_MemRef` constraint in `hivm_dialect.td` requires `f32`. Both have to change together, along with `builder.getF32Type()` in the two emitters and `f32_bytes` in the unified-buffer budget.

## Tests

`lit/*.mim` runs `%mim %s -X ascend:o=-` and FileChecks the text or the diagnostic. `add.mim` and `mix.mim` are HFusion; the `*_hivm.mim` files are HIVM. Golden tests bind SSA names with FileCheck variables rather than spelling them out, because MLIR picks those names and has changed them between releases. `reject_hivm_ub.mim` passes `-X ascend:ub=191` so the test does not depend on the default capacity: two inputs and one result, 16 F32 elements, is 192 bytes.

After editing a test, stage it before trusting `lit`:

```sh
cmake --build build --target mim_lit_tests
python3 lit/lit build/lit -a --filter 'extra/ascend'
```

## Build

The plugin needs MLIR 19, 20 or 21; CMake probes `/usr/lib/llvm-*/lib/cmake/mlir` and honours `MLIR_DIR`. Without a usable MLIR the plugin is skipped with a status message, and the `lit` tests under `extra/ascend/lit` then fail at `plugin ascend`. The upper bound is `linalg.elemwise_binary`, which MLIR 22 replaced with `linalg.elementwise`; `bishengir` is built on LLVM 19 and parses the older spelling, so following MLIR 22 here would break the HFusion comparison path rather than fix it. Everything else in the plugin, the TableGen dialects included, already compiles against MLIR 22.

`mlir-tblgen` generates `<dialect>_{enums,dialect,attrs,ops}.{h,cpp}.inc` into the plugin's build directory; the `mim_ascend_hacc_inc` and `mim_ascend_hivm_inc` targets own that step. The plugin links `MLIRFuncDialect`, `MLIRLinalgDialect`, `MLIRMemRefDialect`, `MLIRTensorDialect`, `MLIRIR` and `MLIRSupport`, and its rpath includes the LLVM library directory so the shared LLVM resolves when `mim` loads the plugin.

`bishengir-compile -enable-hivm-compile` parses the printed module and rewrites `hivm.hir.vadd` into library calls plus `set_flag` / `wait_flag`. On the CANN 9.1.0 image used while writing this, that link then fails with `undefined symbol: _mlir_ciface_vadd_1d_float`. The same error happens for a handwritten copy of the documented VecAdd kernel, so it is the bitcode link in that install, not a spelling bug in `build`.
