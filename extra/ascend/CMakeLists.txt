# The plugin builds every module with the MLIR C++ API, so without MLIR there is nothing to build.
# MLIR 22 dropped linalg.elemwise_binary, which is what the HFusion path emits and what bishengir parses.
set(MIM_ASCEND_MLIR_MIN 19)
set(MIM_ASCEND_MLIR_MAX 21)

if(NOT MLIR_DIR)
    foreach(ver RANGE ${MIM_ASCEND_MLIR_MIN} ${MIM_ASCEND_MLIR_MAX})
        if(EXISTS "/usr/lib/llvm-${ver}/lib/cmake/mlir/MLIRConfig.cmake")
            set(MLIR_DIR "/usr/lib/llvm-${ver}/lib/cmake/mlir")
            break()
        endif()
    endforeach()
endif()

find_package(MLIR CONFIG)
if(NOT MLIR_FOUND)
    message(STATUS "MimIR ascend: MLIR not found; skipping the plugin. Set MLIR_DIR to build it.")
    return()
elseif(MLIR_VERSION_MAJOR LESS MIM_ASCEND_MLIR_MIN OR MLIR_VERSION_MAJOR GREATER MIM_ASCEND_MLIR_MAX)
    message(STATUS
        "MimIR ascend: skipping the plugin; MLIR ${MLIR_VERSION} was found but "
        "${MIM_ASCEND_MLIR_MIN} to ${MIM_ASCEND_MLIR_MAX} is required. Set MLIR_DIR to one of those."
    )
    return()
endif()

# MLIRConfig only names the tool, so resolve it against the installation the dialect libraries come from.
if(NOT EXISTS "${MLIR_TABLEGEN_EXE}")
    find_program(MIM_ASCEND_MLIR_TBLGEN mlir-tblgen REQUIRED HINTS "${LLVM_TOOLS_BINARY_DIR}")
    set(MLIR_TABLEGEN_EXE "${MIM_ASCEND_MLIR_TBLGEN}")
endif()

list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}" "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)

# mlir_tablegen derives its -I flags from this directory's include path.
include_directories(SYSTEM ${MLIR_INCLUDE_DIRS})

# Generates <dialect>_{enums,dialect,attrs,ops}.{h,cpp}.inc below ${CMAKE_CURRENT_BINARY_DIR}.
function(mim_ascend_dialect dialect)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "OPS" "" "")

    set(LLVM_TARGET_DEFINITIONS "${dialect}_dialect.td")
    mlir_tablegen("${dialect}_enums.h.inc" -gen-enum-decls)
    mlir_tablegen("${dialect}_enums.cpp.inc" -gen-enum-defs)
    mlir_tablegen("${dialect}_dialect.h.inc" -gen-dialect-decls -dialect=${dialect})
    mlir_tablegen("${dialect}_dialect.cpp.inc" -gen-dialect-defs -dialect=${dialect})
    mlir_tablegen("${dialect}_attrs.h.inc" -gen-attrdef-decls -attrdefs-dialect=${dialect})
    mlir_tablegen("${dialect}_attrs.cpp.inc" -gen-attrdef-defs -attrdefs-dialect=${dialect})
    if(ARG_OPS)
        mlir_tablegen("${dialect}_ops.h.inc" -gen-op-decls)
        mlir_tablegen("${dialect}_ops.cpp.inc" -gen-op-defs)
    endif()

    add_public_tablegen_target(mim_ascend_${dialect}_inc)
endfunction()

mim_ascend_dialect(hacc)
mim_ascend_dialect(hivm OPS)

add_mim_plugin(ascend
    SOURCES
        ascend.cpp
        emit.cpp
        hacc_dialect.cpp
        hfusion.cpp
        hivm.cpp
        hivm_dialect.cpp
        kernel.cpp
)

add_dependencies(mim_ascend mim_ascend_hacc_inc mim_ascend_hivm_inc)
# The generated .inc files are included as system headers so -Wall -Wextra stays about hand-written code.
target_include_directories(mim_ascend SYSTEM PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
separate_arguments(ASCEND_LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
target_compile_options(mim_ascend PRIVATE ${ASCEND_LLVM_DEFS})
target_link_libraries(mim_ascend PRIVATE
    MLIRFuncDialect
    MLIRIR
    MLIRLinalgDialect
    MLIRMemRefDialect
    MLIRSupport
    MLIRTensorDialect
)

# The plugin loads at runtime, so it has to find libLLVM/libMLIR without a wrapper script.
if(LLVM_LIBRARY_DIRS)
    set_target_properties(mim_ascend PROPERTIES
        BUILD_RPATH "${LLVM_LIBRARY_DIRS}"
        INSTALL_RPATH "${LLVM_LIBRARY_DIRS}"
    )
endif()
