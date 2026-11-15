add_library(pbqp STATIC)
target_sources(pbqp
    PRIVATE
        dot.cpp
        graph.cpp
        json.cpp
        solver.cpp
)
target_include_directories(pbqp
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(pbqp PUBLIC cxx_std_23)
find_package(Threads REQUIRED)
# fe's objects are compiled into libmim, so pbqp resolves fe's symbols there instead of carrying a copy.
target_link_libraries(pbqp
    PUBLIC
        $<BUILD_INTERFACE:fe_usage>
        $<INSTALL_INTERFACE:fe::fe>
        Threads::Threads
)

# A static x86 plugin carries pbqp as a link dependency in the export set.
if(NOT BUILD_SHARED_LIBS)
    install(
        TARGETS pbqp
        EXPORT mim-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
