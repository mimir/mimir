set(CLI_HELP   "${CMAKE_CURRENT_BINARY_DIR}/cli-help.sh")
set(XDOT_HELP  "${CMAKE_CURRENT_BINARY_DIR}/xdot-help.gdb")
set(MIM_LOGO   "${CMAKE_CURRENT_BINARY_DIR}/logo.svg")
set(ITER_DOT   "${CMAKE_CURRENT_BINARY_DIR}/iter.dot")
set(COUNT_DOT  "${CMAKE_CURRENT_BINARY_DIR}/count.dot")
set(SQ_DOT     "${CMAKE_CURRENT_BINARY_DIR}/sq.dot")
set(DEP_DOT    "${CMAKE_CURRENT_BINARY_DIR}/dep.dot")

# The tour graphs are small and fit on the page, so we embed them as *static* SVGs
# (via @image html) for tight margins. INTERACTIVE_SVG stays on for the larger
# auto-generated API graphs, which benefit from doxygen's pan/zoom viewer.
set(ITER_SVG   "${CMAKE_CURRENT_BINARY_DIR}/iter.svg")
set(COUNT_SVG  "${CMAKE_CURRENT_BINARY_DIR}/count.svg")
set(SQ_SVG     "${CMAKE_CURRENT_BINARY_DIR}/sq.svg")
set(DEP_SVG    "${CMAKE_CURRENT_BINARY_DIR}/dep.svg")

# Single source of truth: the .mim examples live in lit/docs so they double as
# regularly-run lit regression tests; the docs include them and render their graphs.
set(LIT_DOCS   "${PROJECT_SOURCE_DIR}/lit/docs")

add_custom_command(
    OUTPUT ${CLI_HELP}
    COMMAND mim --help > ${CLI_HELP}
    DEPENDS mim
    COMMENT "Generating help text of Mim's CLI"
)

add_custom_command(
    OUTPUT ${ITER_SVG}
    COMMAND mim lit/docs/iter.mim --dot-inline --dot-hide-default-filter --dot-show-detached --output-dot ${ITER_DOT}
    COMMAND ${DOXYGEN_DOT_EXECUTABLE} -Tsvg ${ITER_DOT} -o ${ITER_SVG}
    DEPENDS mim "${LIT_DOCS}/iter.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR graph for the iter example in docs/mimir.md"
)

add_custom_command(
    OUTPUT ${COUNT_SVG}
    COMMAND mim lit/docs/count.mim --dot-inline --dot-hide-default-filter --dot-show-detached --output-dot ${COUNT_DOT}
    COMMAND ${DOXYGEN_DOT_EXECUTABLE} -Tsvg ${COUNT_DOT} -o ${COUNT_SVG}
    DEPENDS mim "${LIT_DOCS}/count.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR graph for the count example in docs/mimir.md"
)

add_custom_command(
    OUTPUT ${SQ_SVG}
    COMMAND mim lit/docs/sq.mim --dot-inline --dot-hide-default-filter --dot-show-detached --output-dot ${SQ_DOT}
    COMMAND ${DOXYGEN_DOT_EXECUTABLE} -Tsvg ${SQ_DOT} -o ${SQ_SVG}
    DEPENDS mim "${LIT_DOCS}/sq.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR graph for the sq example in docs/README.md"
)

add_custom_command(
    OUTPUT ${DEP_SVG}
    COMMAND mim lit/docs/dep.mim --dot-inline --dot-hide-default-filter --dot-show-detached --dot-follow-types --output-dot ${DEP_DOT}
    COMMAND ${DOXYGEN_DOT_EXECUTABLE} -Tsvg ${DEP_DOT} -o ${DEP_SVG}
    DEPENDS mim "${LIT_DOCS}/dep.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR graph for the dependent-types example in docs/mimir.md"
)

add_custom_command(
    OUTPUT ${XDOT_HELP}
    COMMAND gdb --silent -x docs/generate_xdot_help.gdb > ${XDOT_HELP}
    DEPENDS ../scripts/xdot.gdb generate_xdot_help.gdb
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating GDB xdot help text"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../assets/logo.svg"
    "${MIM_LOGO}"
    COPYONLY)

set(DOXYFILE        "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")
set(DOXY_LAYOUT     "${CMAKE_CURRENT_BINARY_DIR}/DoxygenLayout.xml")
set(DOXY_INDEX_HTML "${CMAKE_CURRENT_BINARY_DIR}/html/index.html")
set(DOXY_EXTRA_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/mimir.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/coding.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/langref.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/python.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/rewriting.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/phases.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
)

string(REPLACE ";" " " DOXY_EXTRA_FILES_CONFIG "${DOXY_EXTRA_FILES}")

configure_file(Doxyfile.in ${DOXYFILE} @ONLY)
configure_file(DoxygenLayout.xml.in ${DOXY_LAYOUT} @ONLY)

add_custom_command(
    OUTPUT ${DOXY_INDEX_HTML}
    COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYFILE}
    MAIN_DEPENDENCY ${DOXYFILE}
    DEPENDS mim ${CLI_HELP} ${XDOT_HELP} ${ITER_SVG} ${COUNT_SVG} ${SQ_SVG} ${DEP_SVG} ${DOXY_EXTRA_FILES} ${DOXY_LAYOUT} ${MIM_LOGO}
    COMMENT "Generating Doxygen HTML documentation"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)

list(TRANSFORM MIM_PLUGIN_LIST PREPEND mim_internal_ OUTPUT_VARIABLE INTERNAL_LIST)
add_custom_target(docs
    ALL
    DEPENDS
        ${DOXY_INDEX_HTML}
        ${INTERNAL_LIST}
)

add_dependencies(mim_all docs)
