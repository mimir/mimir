set(CLI_HELP   "${CMAKE_CURRENT_BINARY_DIR}/cli-help.sh")
set(XDOT_HELP  "${CMAKE_CURRENT_BINARY_DIR}/xdot-help.gdb")
set(MIM_LOGO   "${CMAKE_CURRENT_BINARY_DIR}/logo.svg")
set(LIT_DOCS   "${PROJECT_SOURCE_DIR}/lit/docs") # the docs include them and render their graphs.

add_custom_command(
    OUTPUT ${CLI_HELP}
    COMMAND mim --help > ${CLI_HELP}
    DEPENDS mim
    COMMENT "Generating help text of Mim's CLI"
)

# The tour graphs are small and fit on the page, so we render them to *static* SVGs
# (embedded via @image html) for tight margins.
set(DOC_GRAPH_FLAGS --dot-inline-consts --dot-show-hidden)
set(dep_EXTRA_FLAGS --dot-follow-types)

set(DOC_GRAPH_SVGS "")
foreach(graph IN ITEMS count iter sq dep)
    set(dot "${CMAKE_CURRENT_BINARY_DIR}/${graph}.dot")
    set(svg "${CMAKE_CURRENT_BINARY_DIR}/${graph}.svg")
    add_custom_command(
        OUTPUT ${svg}
        COMMAND mim lit/docs/${graph}.mim ${DOC_GRAPH_FLAGS} ${${graph}_EXTRA_FLAGS} --output-dot ${dot}
        COMMAND ${DOXYGEN_DOT_EXECUTABLE} -Tsvg ${dot} -o ${svg}
        DEPENDS mim mim_all_plugins "${LIT_DOCS}/${graph}.mim"
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating MimIR graph for the ${graph} example"
    )
    list(APPEND DOC_GRAPH_SVGS ${svg})
endforeach()

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
    DEPENDS mim ${CLI_HELP} ${XDOT_HELP} ${DOC_GRAPH_SVGS} ${DOXY_EXTRA_FILES} ${DOXY_LAYOUT} ${MIM_LOGO}
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
