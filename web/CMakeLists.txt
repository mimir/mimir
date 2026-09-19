# Stages page and Emscripten artifacts side by side, so that serving
# ${CMAKE_BINARY_DIR}/playground over HTTP is all it takes to run it.
set(PLAYGROUND_DIR ${CMAKE_BINARY_DIR}/playground)

set(PLAYGROUND_SOURCES
    index.html
    playground.css
    playground.js
)

set(PLAYGROUND_STAGED "")
foreach(FILE IN LISTS PLAYGROUND_SOURCES)
    add_custom_command(
        OUTPUT  ${PLAYGROUND_DIR}/${FILE}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_LIST_DIR}/${FILE} ${PLAYGROUND_DIR}/${FILE}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/${FILE}
        COMMENT "Staging playground file '${FILE}'"
        VERBATIM
    )
    list(APPEND PLAYGROUND_STAGED ${PLAYGROUND_DIR}/${FILE})
endforeach()

# mim.data holds the plugins' `.mim` halves and the ll runtime; mim.js loads both siblings by name.
foreach(ARTIFACT mim.js mim.wasm mim.data)
    add_custom_command(
        OUTPUT  ${PLAYGROUND_DIR}/${ARTIFACT}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:mim>/${ARTIFACT} ${PLAYGROUND_DIR}/${ARTIFACT}
        DEPENDS mim
        COMMENT "Staging playground artifact '${ARTIFACT}'"
        VERBATIM
    )
    list(APPEND PLAYGROUND_STAGED ${PLAYGROUND_DIR}/${ARTIFACT})
endforeach()

add_custom_target(mim_playground ALL DEPENDS ${PLAYGROUND_STAGED})
