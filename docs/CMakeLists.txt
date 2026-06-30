set(CLI_HELP   "${CMAKE_CURRENT_BINARY_DIR}/cli-help.sh")
set(XDOT_HELP  "${CMAKE_CURRENT_BINARY_DIR}/xdot-help.gdb")
set(MIM_LOGO   "${CMAKE_CURRENT_BINARY_DIR}/logo.svg")
set(ITER_DOT   "${CMAKE_CURRENT_BINARY_DIR}/iter.dot")
set(COUNT_DOT  "${CMAKE_CURRENT_BINARY_DIR}/count.dot")
set(SQ_DOT     "${CMAKE_CURRENT_BINARY_DIR}/sq.dot")
set(DEP_DOT    "${CMAKE_CURRENT_BINARY_DIR}/dep.dot")

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
    OUTPUT ${ITER_DOT}
    COMMAND mim lit/docs/iter.mim --dot-inline --dot-hide-default-filter --output-dot ${ITER_DOT}
    DEPENDS mim "${LIT_DOCS}/iter.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR DOT graph for the iter example in docs/mimir.md"
)

add_custom_command(
    OUTPUT ${COUNT_DOT}
    COMMAND mim lit/docs/count.mim --dot-inline --dot-hide-default-filter --output-dot ${COUNT_DOT}
    DEPENDS mim "${LIT_DOCS}/count.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR DOT graph for the count example in docs/mimir.md"
)

add_custom_command(
    OUTPUT ${SQ_DOT}
    COMMAND mim lit/docs/sq.mim --dot-inline --dot-hide-default-filter --output-dot ${SQ_DOT}
    DEPENDS mim "${LIT_DOCS}/sq.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR DOT graph for the sq example in docs/README.md"
)

add_custom_command(
    OUTPUT ${DEP_DOT}
    COMMAND mim lit/docs/dep.mim --dot-inline --dot-hide-default-filter --dot-follow-types --output-dot ${DEP_DOT}
    DEPENDS mim "${LIT_DOCS}/dep.mim"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Generating MimIR DOT graph for the dependent-types example in docs/mimir.md"
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
    DEPENDS mim ${CLI_HELP} ${XDOT_HELP} ${ITER_DOT} ${COUNT_DOT} ${SQ_DOT} ${DEP_DOT} ${DOXY_EXTRA_FILES} ${DOXY_LAYOUT} ${MIM_LOGO}
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
