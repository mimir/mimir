# Standalone smoke tests for the MLIR backend region tree / printer / ops.
# The same sources are compiled into the core plugin (a MODULE), so we
# re-compile them here rather than linking against the plugin .so.
set(MLIR_BE_SRC
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/region_tree.cpp
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/printer.cpp
        ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/type_converter.cpp
)

add_executable(mlir_test_types  test_types.cpp  ${MLIR_BE_SRC})
add_executable(mlir_test_linalg test_linalg.cpp ${MLIR_BE_SRC})
add_executable(mlir_test_typeconv test_typeconv.cpp  ${MLIR_BE_SRC})

target_link_libraries(mlir_test_types  PRIVATE libmim)
target_link_libraries(mlir_test_linalg PRIVATE libmim)
target_link_libraries(mlir_test_typeconv PRIVATE libmim)

add_dependencies(mim_all mlir_test_types mlir_test_linalg mlir_test_typeconv)
