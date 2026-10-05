# Building and running the Ascend lowering

This note is how to build MimIR with the `extra/ascend` plugin on the Huawei server, emit MLIR for an F32 tensor add, check that emission with the `lit` tests, and compile the MLIR with `bishengir-compile`.

The plugin lowers one direct-style external function: a static same-shape F32 `tensor.binary` addition. The phase is `ascend.emit_hfusion`. It writes a `func.func` that uses `tensor.empty` and `linalg.elemwise_binary`, marked `hacc.entry` and `#hacc.function_kind<DEVICE>`. There is no host launcher yet, so this path stops at a device object file.

Verified on this machine on 16 October 2026:

- CANN `9.1.0` at `/usr/local/Ascend/cann-9.1.0`
- `bishengir-compile 1.2.0` (LLVM 19.1.7)
- NPU 0: Ascend950PR, reported healthy by `npu-smi`
- Host compiler used for MimIR: GCC 14.3.0, CMake 4.4.4

## What the image is missing

The server image already has CANN, `bishengir-compile`, and `bishengir-opt` on `PATH`. It does not have a new enough host toolchain for MimIR:

| Requirement | Why | What the image has |
| --- | --- | --- |
| CMake 3.25 or newer | `CMakeLists.txt` requires 3.25 | no `cmake` on `PATH`; Ubuntu 22.04's package is 3.22 |
| A C++23 compiler with `std::format` and `std::print` | MimIR is C++23 | GCC 11.4, which is not enough. Use GCC 14 |
| Git submodules | `abseil-cpp` and `fe` are required to compile | present in `.gitmodules`, not checked out until you init them |
| `FileCheck` and the LLVM `lit` runner | the `extra/ascend/lit` tests | install `llvm-14-tools` and `python3-psutil` |

Python bindings are not needed to build or run the Ascend plugin.

## 1. Host packages

```sh
sudo apt-get update
sudo apt-get install -y software-properties-common gnupg gpg-agent
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test
sudo apt-get update
sudo apt-get install -y g++-14 gcc-14 llvm-14-tools python3-psutil
```

`add-apt-repository` needs `gpg-agent`. On this image it fails with `can't connect to the agent` until `gnupg` and `gpg-agent` are installed.

CMake from Ubuntu is too old. The Huawei PyPI mirror already configured for `/usr/local/python3.12.13` can install a current CMake, and that interpreter's `bin` directory is on `PATH`:

```sh
/usr/local/python3.12.13/bin/python3 -m pip install cmake
cmake --version    # 3.25 or newer; 4.4.4 was used here
g++-14 --version
```

`FileCheck` lands at `/usr/bin/FileCheck-14`. The `lit` Python package that ships with LLVM 14 is not on the default `PYTHONPATH`:

```sh
export PYTHONPATH=/usr/lib/llvm-14/build/utils/lit${PYTHONPATH:+:$PYTHONPATH}
```

Put that export in the shell you use for tests. `python3-psutil` is required because the suite sets a per-test timeout.

## 2. Submodules

From the MimIR checkout:

```sh
cd /workspace/mimir
git submodule update --init --recursive
```

That checks out `submodules/abseil-cpp`, `submodules/fe` (including `fe`'s `doctest`), `submodules/nanobind`, and `submodules/doxygen-awesome-css`. GitHub clones from this network sometimes drop the TLS connection; rerun the same command if a submodule fails. A successful run ends with every line of `git submodule status` lacking a leading `-`.

`nanobind` and `doxygen-awesome-css` are unused by the commands below. They are still part of the recursive checkout.

## 3. Configure and build

```sh
export CC=gcc-14 CXX=g++-14
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DMIM_BUILD_PYTHON=OFF \
  -DMIM_BUILD_EXAMPLES=OFF \
  -DMIM_BUILD_DOCS=OFF \
  -DMIM_BUILD_LL_RUNTIME=OFF \
  -DMIM_FILECHECK=/usr/bin/FileCheck-14
cmake --build build -j"$(nproc)"
```

`extra/ascend` is discovered on its own. Configure prints `Adding third-party Mim plugin from 'extra/ascend'`. The build installs the plugin next to the other plugins:

- `build/bin/mim`
- `build/lib/libmim.so`
- `build/lib/mim/libmim_ascend.so`
- `build/lib/mim/ascend.mim`

`MIM_BUILD_PYTHON=OFF` skips the nanobind package. `MIM_BUILD_LL_RUNTIME=OFF` skips the `ll` backend's C runtime, which needs `clang` and is not used by this lowering. `BUILD_TESTING=ON` is what generates the `lit` suite; without it the Ascend tests are not registered.

Confirm the plugin argument is visible:

```sh
./build/bin/mim -p ascend --help
```

The `ascend` section is:

```text
o=<file>, output=<file>    Writes Linalg/HFusion MLIR to <file>; use `-` for stdout.
```

## 4. Emit the add kernel

`extra/ascend/lit/add.mim` is the kernel:

```mim
extern lam add (a b: «1024; math.F32»): «1024; math.F32» =
    tensor.binary (math.arith.add 0 @math.f32) (a, b);

extern lam _compile (): compile.Phase = ascend.emit_hfusion;
```

Print the MLIR:

```sh
./build/bin/mim extra/ascend/lit/add.mim -X ascend:o=-
```

Or write it to a file. `-` is stdout. Any other path is taken as given. With no `-X ascend:o=...`, the phase writes `<world>.ascend.mlir` in the current directory. For this input the world name is `add`, so the file is `add.ascend.mlir`.

The output is:

```mlir
module {
  func.func @add(%arg0: tensor<1024xf32>, %arg1: tensor<1024xf32>) -> tensor<1024xf32> attributes {hacc.entry, hacc.function_kind = #hacc.function_kind<DEVICE>} {
    %empty0 = tensor.empty() : tensor<1024xf32>
    %result0 = linalg.elemwise_binary {fun = #linalg.binary_fn<add>} ins(%arg0, %arg1 : tensor<1024xf32>, tensor<1024xf32>) outs(%empty0 : tensor<1024xf32>) -> tensor<1024xf32>
    return %result0 : tensor<1024xf32>
  }
}
```

An unsupported program is rejected instead of emitting IR. `extra/ascend/lit/reject_i32.mim` is an I32 add:

```sh
./build/bin/mim extra/ascend/lit/reject_i32.mim -X ascend:o=-
```

That exits non-zero and reports `error: ascend backend only supports F32 tensor elements`.

## 5. Run the lit tests

Stage the suite once, then run only the Ascend tests:

```sh
export PYTHONPATH=/usr/lib/llvm-14/build/utils/lit${PYTHONPATH:+:$PYTHONPATH}
cmake --build build --target mim_lit_tests
python3 lit/lit build/lit -a --filter 'extra/ascend'
```

Both tests passed in the build used to write this note:

```text
PASS: mim regression :: extra/ascend/add.mim
PASS: mim regression :: extra/ascend/reject_i32.mim
```

`cmake --build build --target lit` runs the whole staged suite (449 tests after staging, including these two). The `lit` target depends on `mim_lit_tests`, so the separate staging step is unnecessary if you invoke `lit`.

The tests live in `extra/ascend/lit/` and are copied to `build/lit/tests/extra/ascend/` when the suite is staged. Edit the copies under `extra/ascend/lit/`, then rebuild the `mim_lit_tests` target before rerunning.

## 6. Compile the MLIR with BiShengIR

Source CANN if `bishengir-compile` is not already on `PATH`:

```sh
source /usr/local/Ascend/cann-9.1.0/set_env.sh
```

Compile the file emitted above:

```sh
./build/bin/mim extra/ascend/lit/add.mim -X ascend:o=add.mlir
bishengir-compile add.mlir --enable-hfusion-compile --target=Ascend950PR_9579 -o add.o
```

`--target` has to match the card. `npu-smi info -t board -i 0` on this machine reports chip `Ascend950PR` and NPU name `9579`, which is `Ascend950PR_9579` (`dav-c310`). Omitting `--target` still writes an `add.o`, but that object is built against the generic AI-vector bitcode and faults on this card with a vector-core exception (`507035`).

`bishengir-compile 1.2.0` accepts this MLIR. It writes two files:

- `add.o` — the device object. `readelf -h` reports `e_machine` `0x1029`. The object contains `.text`, `.ascend.stack.size.record`, and `__CCE_KernelArgSize`, and the kernel symbols `add$local` and `add__`.
- `libadd.so` — an x86-64 host companion. One exported symbol is `add_get_tiling_struct_size_function`.

`bishengir-compile` may print a deprecation warning for `--cce-aicore-input-parameter-size` and a warning about overriding the module triple with `x86_64-unknown-linux-gnu`. Those warnings did not fail the compile; the device object above was still produced.

`bishengir-opt add.mlir --convert-linalg-to-hfusion` parses the file and exits 0, and it leaves `linalg.elemwise_binary` in the output. The end-to-end HFusion compile does not depend on running that pass by hand.

The targeted object does run. Loaded as a vector-core ELF and launched on the vector engine with three device pointers, the 1024-element kernel returns `out[i] = a[i] + b[i]`. `libadd.so` does not do that launch, and `extra/ascend` still has no AscendCL host. `aclrtBinaryLoadFromFile` rejects the vector-core magic; `aclrtBinaryLoadFromData` with `ACL_RT_BINARY_MAGIC_ELF_VECTOR_CORE` accepts it. A plain `aclrtLaunchKernel` schedules the task on a cube core, which then faults.

## Rebuild after editing the plugin

```sh
cmake --build build -j"$(nproc)"
```

CMake tracks `extra/ascend/ascend.cpp`. A change to `ascend.mim` is picked up by the plugin's bootstrap step on the next build. After either change, rerun the `lit` filter from section 5 before trusting a new `bishengir-compile` run.
