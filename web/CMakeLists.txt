# `mim-worker.js` calls createMim(), then FS and callMain.
# The plugins are linked in, but their `.mim` halves and the ll runtime are found at runtime.
target_link_options(mim
    PRIVATE
        --preload-file "${MIM_PLUGIN_DIR}@/mim"
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=16MB # a recursive frontend on the default 64KB stack overflows
        -sFORCE_FILESYSTEM=1
        -sINVOKE_RUN=0 # the embedder calls main once per run
        -sMODULARIZE=1
        -sEXPORT_NAME=createMim
        -sEXPORTED_RUNTIME_METHODS=callMain,FS
)
add_dependencies(mim mim_all_plugins) # --preload-file needs the `.mim` halves staged first

# Page and Emscripten artifacts side by side: serving this directory over HTTP runs the playground.
set(PLAYGROUND_DIR ${CMAKE_BINARY_DIR}/playground)

set(PLAYGROUND_STAGED "")

# DEP defaults to SRC; pass a target for a generated file.
function(mim_stage_playground SRC DST)
    set(DEP ${ARGN})
    if(NOT DEP)
        set(DEP ${SRC})
    endif()
    add_custom_command(
        OUTPUT  ${PLAYGROUND_DIR}/${DST}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SRC} ${PLAYGROUND_DIR}/${DST}
        DEPENDS ${DEP}
        COMMENT "Staging playground file '${DST}'"
        VERBATIM
    )
    set(PLAYGROUND_STAGED ${PLAYGROUND_STAGED} ${PLAYGROUND_DIR}/${DST} PARENT_SCOPE)
endfunction()

foreach(FILE IN ITEMS index.html playground.css playground.js mim-worker.js)
    mim_stage_playground(${CMAKE_CURRENT_LIST_DIR}/${FILE} ${FILE})
endforeach()

# The docs' dark-mode toggle, so the manual and the playground share one control and one stored preference.
mim_stage_playground(
    ${PROJECT_SOURCE_DIR}/submodules/doxygen-awesome-css/doxygen-awesome-darkmode-toggle.js
    darkmode-toggle.js
)

# The docs own the lexers; the page uses the very same ones. `mim.js` is Emscripten's, hence the rename.
mim_stage_playground(${PROJECT_SOURCE_DIR}/docs/code.js code.js)
mim_stage_playground(${PROJECT_SOURCE_DIR}/docs/mim.js mim-code.js)
mim_stage_playground(${PROJECT_SOURCE_DIR}/docs/llvm.js llvm-code.js)

# The examples are the lit suite's, so they cannot rot.
foreach(EXAMPLE IN LISTS MIM_PLAYGROUND_EXAMPLES)
    mim_stage_playground(${PROJECT_SOURCE_DIR}/lit/${EXAMPLE}.mim examples/${EXAMPLE}.mim)
endforeach()

# The picker reads the very list that stages the files.
string(JOIN "\", \"" EXAMPLE_NAMES ${MIM_PLAYGROUND_EXAMPLES})
file(GENERATE OUTPUT ${PLAYGROUND_DIR}/examples/index.json CONTENT "[\"${EXAMPLE_NAMES}\"]")

# mim.js loads its two siblings by name.
foreach(ARTIFACT IN ITEMS mim.js mim.wasm mim.data)
    mim_stage_playground($<TARGET_FILE_DIR:mim>/${ARTIFACT} ${ARTIFACT} mim)
endforeach()

add_custom_target(mim_playground ALL DEPENDS ${PLAYGROUND_STAGED})

# CI runs this, too.
find_program(MIM_NODE NAMES node)
if(MIM_NODE)
    add_custom_target(mim_playground_smoke
        COMMAND
            ${MIM_NODE} ${CMAKE_CURRENT_LIST_DIR}/smoke.js
            $<TARGET_FILE_DIR:mim> ${PROJECT_SOURCE_DIR}/lit/docs/count.mim
        DEPENDS mim_playground
        COMMENT "Smoke-testing the wasm mim"
        VERBATIM
    )
endif()
