# Standalone smoke tests for the MLIR backend region tree / printer / ops.
set(MLIR_BE_SRC
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/region_tree.cpp
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/printer.cpp
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/type_converter.cpp
    ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/lam_classifier.cpp
        ${PROJECT_SOURCE_DIR}/src/mim/plug/core/be/mlir/mlir_emitter.cpp

)

add_executable(mlir_test_types  test_types.cpp  ${MLIR_BE_SRC})
add_executable(mlir_test_linalg test_linalg.cpp ${MLIR_BE_SRC})
add_executable(mlir_test_typeconv test_typeconv.cpp  ${MLIR_BE_SRC})
add_executable(mlir_test_classifier test_classifier.cpp ${MLIR_BE_SRC})
add_executable(mlir_test_emitter_p2 test_emitter_p2.cpp ${MLIR_BE_SRC})

# MIM_SOURCE_DIR lets test executables find .mim files at runtime
target_compile_definitions(mlir_test_classifier PRIVATE
    MIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(mlir_test_emitter_p2 PRIVATE
    MIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

target_link_libraries(mlir_test_emitter_p2 PRIVATE libmim)
target_link_libraries(mlir_test_classifier PRIVATE libmim)
target_link_libraries(mlir_test_types  PRIVATE libmim)
target_link_libraries(mlir_test_linalg PRIVATE libmim)
target_link_libraries(mlir_test_typeconv PRIVATE libmim)

add_dependencies(mim_all mlir_test_types mlir_test_linalg mlir_test_typeconv mlir_test_classifier mlir_test_emitter_p2)
