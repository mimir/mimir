find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
add_subdirectory(bindings)

set(MIM_PY_VENV "${CMAKE_BINARY_DIR}/.venv")
if(WIN32)
    set(MIM_PY_PYTHON "${MIM_PY_VENV}/Scripts/python.exe")
else()
    set(MIM_PY_PYTHON "${MIM_PY_VENV}/bin/python")
endif()
set(MIM_PY_STAGE "${CMAKE_BINARY_DIR}/mim_py_stage/main")

execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
            "import os, sys; print(os.path.realpath(getattr(sys, '_base_executable', sys.executable) or sys.executable))"
    OUTPUT_VARIABLE MIM_PY_BOOTSTRAP_PYTHON
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT EXISTS "${MIM_PY_BOOTSTRAP_PYTHON}")
    message(FATAL_ERROR
        "Could not determine a bootstrap Python interpreter from ${Python_EXECUTABLE}"
    )
endif()

# venv: created once, kept between builds. Stamp gates downstream targets.
set(MIM_PY_VENV_STAMP "${MIM_PY_VENV}/.mim-stamp")
add_custom_command(
    OUTPUT "${MIM_PY_VENV_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MIM_PY_VENV}"
    COMMAND $<$<PLATFORM_ID:Windows>:${MIM_PY_BOOTSTRAP_PYTHON}> -m ensurepip --upgrade
    COMMAND "${MIM_PY_BOOTSTRAP_PYTHON}" -m venv "${MIM_PY_VENV}"
    COMMAND "${MIM_PY_PYTHON}" -m pip install --upgrade pip setuptools wheel
    COMMAND "${MIM_PY_PYTHON}" -m pip install pytest
    COMMAND "${CMAKE_COMMAND}" -E touch "${MIM_PY_VENV_STAMP}"
    COMMENT "mim_py: creating venv at ${MIM_PY_VENV}"
    VERBATIM
)
add_custom_target(mim_py_venv DEPENDS "${MIM_PY_VENV_STAMP}")

# Generate a stub for the internal nanobind module.
set(MIM_PY_STUBS_DIR "${CMAKE_BINARY_DIR}/mim_py_stubs")
set(MIM_PY_STUB_FILE "${MIM_PY_STUBS_DIR}/_mim.pyi")
set(MIM_PY_NANOBIND_STUBGEN "${CMAKE_SOURCE_DIR}/submodules/nanobind/src/stubgen.py")
set(_mim_py_stub_runtime_commands)
if(WIN32)
    set(_mim_py_stub_runtime_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:pymim>
                "${MIM_PY_STUBS_DIR}"
    )
endif()
add_custom_command(
    OUTPUT "${MIM_PY_STUB_FILE}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MIM_PY_STUBS_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MIM_PY_STUBS_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy
            "$<TARGET_FILE:pymim>"
            "${MIM_PY_STUBS_DIR}/$<TARGET_FILE_NAME:pymim>"
    ${_mim_py_stub_runtime_commands}
    COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=${MIM_PY_STUBS_DIR}"
            "${MIM_PY_PYTHON}" "${MIM_PY_NANOBIND_STUBGEN}"
                -q
                -m _mim
                -o "${MIM_PY_STUB_FILE}"
    DEPENDS pymim "${MIM_PY_VENV_STAMP}" "${MIM_PY_NANOBIND_STUBGEN}"
    COMMENT "mim_py: generating _mim.pyi via nanobind stubgen"
    COMMAND_EXPAND_LISTS
    VERBATIM
)
add_custom_target(mim_py_stubs DEPENDS "${MIM_PY_STUB_FILE}")

# Stage the package (sources + generated artifacts) and install it editable.
# Editable install lets later builds refresh staged files without re-running pip.
set(_mim_py_plugin_stage "${MIM_PY_STAGE}/src/mim/_plugins")
set(_mim_py_main_runtime_commands)
if(WIN32)
    set(_mim_py_main_runtime_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:pymim>
                "${MIM_PY_STAGE}/src/mim"
    )
endif()
set(_mim_py_stage_plugin_commands
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_mim_py_plugin_stage}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_mim_py_plugin_stage}/__init__.py"
)
foreach(_plugin ${MIM_PLUGIN_LIST})
    list(APPEND _mim_py_stage_plugin_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim/${_plugin}.py"
                "${_mim_py_plugin_stage}/${_plugin}.py"
    )
endforeach()
add_custom_target(mim_py_main
    # Wipe the source side of the stage so deleted py/src files don't linger.
    # Generated artifacts under src/mim/_plugins are restaged below.
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${MIM_PY_STAGE}/src" "${MIM_PY_STAGE}/tests"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}" "${MIM_PY_STAGE}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:pymim>"
            "${MIM_PY_STAGE}/src/mim/$<TARGET_FILE_NAME:pymim>"
    ${_mim_py_main_runtime_commands}
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MIM_PY_STUB_FILE}"
            "${MIM_PY_STAGE}/src/mim/_mim.pyi"
    ${_mim_py_stage_plugin_commands}
    COMMAND "${MIM_PY_PYTHON}" -m pip install --quiet --no-deps -e "${MIM_PY_STAGE}"
    COMMENT "mim_py: staging & installing mim (editable)"
    COMMAND_EXPAND_LISTS
    VERBATIM
)
add_dependencies(mim_py_main pymim mim_all_plugins mim_py_venv mim_py_stubs)

# Editable installs for third-party plugins under py/plugins/*/pyproject.toml.
file(GLOB _mim_py_plugin_tomls CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*/pyproject.toml")
set(_mim_py_plugin_targets)
foreach(_toml ${_mim_py_plugin_tomls})
    get_filename_component(_plugin_dir  "${_toml}" DIRECTORY)
    get_filename_component(_plugin_name "${_plugin_dir}" NAME)
    set(_target mim_py_plugin_${_plugin_name})
    set(_stage "${CMAKE_BINARY_DIR}/mim_py_stage/plugins/${_plugin_name}")
    add_custom_target(${_target}
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_plugin_dir}" "${_stage}"
        COMMAND "${MIM_PY_PYTHON}" -m pip install --quiet --no-deps -e "${_stage}"
        COMMENT "mim_py: staging & installing ${_target} (editable)"
        VERBATIM
    )
    add_dependencies(${_target} mim_py_main)
    list(APPEND _mim_py_plugin_targets ${_target})
endforeach()

add_custom_target(mim_py ALL)
add_dependencies(mim_py mim_py_main ${_mim_py_plugin_targets})

if(WIN32)
    add_custom_target(test-py
        COMMAND "${CMAKE_COMMAND}" -E env
                "PATH=${CMAKE_RUNTIME_OUTPUT_DIRECTORY};${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim;$ENV{PATH}"
                "${MIM_PY_PYTHON}" -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        DEPENDS mim_py_main
        COMMENT "mim_py: running Python tests"
        USES_TERMINAL
        VERBATIM
    )
else()
    add_custom_target(test-py
        COMMAND "${MIM_PY_PYTHON}" -m pytest "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        DEPENDS mim_py_main
        COMMENT "mim_py: running Python tests"
        USES_TERMINAL
        VERBATIM
    )
endif()

if(TARGET test-all)
    add_dependencies(test-all test-py)
endif()
