# Stages page and Emscripten artifacts side by side, so that serving
# ${CMAKE_BINARY_DIR}/playground over HTTP is all it takes to run it.
set(PLAYGROUND_DIR ${CMAKE_BINARY_DIR}/playground)

set(PLAYGROUND_STAGED "")

function(mim_stage_playground SRC DST DEP)
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
    mim_stage_playground(${CMAKE_CURRENT_LIST_DIR}/${FILE} ${FILE} ${CMAKE_CURRENT_LIST_DIR}/${FILE})
endforeach()

# The docs own the Mim word lists; the page classifies against the very same ones.
mim_stage_playground(${PROJECT_SOURCE_DIR}/docs/mim.js mim-code.js ${PROJECT_SOURCE_DIR}/docs/mim.js)

# The examples are the lit suite's, so they cannot rot; their `RUN:` line is a comment and stays.
foreach(EXAMPLE IN ITEMS sq.mim count.mim dep.mim iter.mim)
    mim_stage_playground(
        ${PROJECT_SOURCE_DIR}/lit/docs/${EXAMPLE} examples/${EXAMPLE}
        ${PROJECT_SOURCE_DIR}/lit/docs/${EXAMPLE}
    )
endforeach()

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
