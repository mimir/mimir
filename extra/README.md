# Third-Party Plugins

Clone third-party plugin repositories here.
Any direct child directory with a `CMakeLists.txt` is added to the main build automatically.
- If such a repository also contains a `lit/` directory, its tests are added to the main lit suite automatically.
- If it contains a `test/` directory with `*.cpp` files, they are built as `mim-<plugin>-test` and added to the main [doctest](https://github.com/doctest/doctest) suite automatically;
  `doctest`'s `main` is generated, and the repository's `include/` directory is on the include path.
- Additional link dependencies can be added from the repository's own `CMakeLists.txt` via `target_link_libraries(mim-<plugin>-test PRIVATE ...)`.
  The plugin itself is a `MODULE` library and must not be linked this way; compile the sources under test into the test target instead:
  ```cmake
  target_sources(mim-<plugin>-test PRIVATE normalizers.cpp)
  target_include_directories(mim-<plugin>-test PRIVATE "${CMAKE_BINARY_DIR}/include") # for autogen.h
  ```
