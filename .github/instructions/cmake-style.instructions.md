---
applyTo: "**/CMakeLists.txt,**/*.cmake"
---

When reviewing CMake files in this repository, treat the following as coding-style requirements:

- Use 4 spaces for one indentation level.
- Prefer lowercase CMake commands such as `set`, `if`, `foreach`, and `add_subdirectory`.
- Prefer uppercase names for project-specific CMake variables and options such as `MIM_BUILD_DOCS` and `MIM_PLUGINS`.
- For longer CMake calls, prefer putting arguments on separate indented lines instead of cramming everything onto one line.
