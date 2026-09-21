---
applyTo: "**/CMakeLists.txt,**/*.cmake"
---

General CMake guidelines for this repository.

- 4 spaces per indentation level.
- Lowercase commands (`set`, `if`, `foreach`, `add_subdirectory`).
- Uppercase project-specific variables and options (`MIM_BUILD_DOCS`, `MIM_PLUGINS`).
- Put the arguments of longer calls on separate indented lines instead of one crammed line.
