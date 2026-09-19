# How `mim` is built for this page: `mim-worker.js` calls createMim(), then FS and callMain.
# The plugins are linked in, but their `.mim` halves and the ll runtime still have to be found at runtime.
target_link_options(mim
    PRIVATE
        --preload-file "${MIM_PLUGIN_DIR}@/mim"
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=16MB # a recursive frontend on the default 64KB stack overflows
        -sFORCE_FILESYSTEM=1
        -sINVOKE_RUN=0 # the embedder calls main once per run, with its own argv
        -sMODULARIZE=1
        -sEXPORT_NAME=createMim
        -sEXPORTED_RUNTIME_METHODS=callMain,FS
)
add_dependencies(mim mim_all_plugins) # --preload-file needs the `.mim` halves staged first

# Stages page and Emscripten artifacts side by side, so that serving
# ${CMAKE_BINARY_DIR}/playground over HTTP is all it takes to run it.
set(PLAYGROUND_DIR ${CMAKE_BINARY_DIR}/playground)

set(PLAYGROUND_STAGED "")

# DEP defaults to SRC; pass a target for a file some other command produces.
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

# The docs own the Mim lexer; the page classifies with the very same one.
mim_stage_playground(${PROJECT_SOURCE_DIR}/docs/mim.js mim-code.js)

# The examples are the lit suite's, so they cannot rot; their `RUN:` line is a comment and stays.
foreach(EXAMPLE IN LISTS MIM_DOC_EXAMPLES)
    mim_stage_playground(${PROJECT_SOURCE_DIR}/lit/docs/${EXAMPLE}.mim examples/${EXAMPLE}.mim)
endforeach()

# The picker reads the very list that stages the files, so the two cannot disagree.
string(JOIN "\", \"" EXAMPLE_NAMES ${MIM_DOC_EXAMPLES})
file(GENERATE OUTPUT ${PLAYGROUND_DIR}/examples/index.json CONTENT "[\"${EXAMPLE_NAMES}\"]")

# mim.data holds the plugins' `.mim` halves and the ll runtime; mim.js loads both siblings by name.
foreach(ARTIFACT IN ITEMS mim.js mim.wasm mim.data)
    mim_stage_playground($<TARGET_FILE_DIR:mim>/${ARTIFACT} ${ARTIFACT} mim)
endforeach()

add_custom_target(mim_playground ALL DEPENDS ${PLAYGROUND_STAGED})

# The same check CI runs; see web/smoke.js.
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
